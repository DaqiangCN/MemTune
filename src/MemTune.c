// MemTune.c - 常驻内存优化器（纯 C + Win32，无 C++ 运行时依赖）
// 编译：gcc MemTune.c default-manifest.o -o MemTune.exe -O2 -mwindows -lpsapi -lshlwapi
//
// 目标：把「不用的内存」压到页面文件，给游戏腾出物理内存
//
// 两种模式：
//   空闲（每 120 秒）：可用内存 < 7GB 时，轻度 trim 大块头后台进程
//   游戏（每 30 秒）  ：检出 games.txt 里的进程，深度 trim 所有非保护进程 + 清 standby
//
// 红线：
//   1. 绝不 NtSuspendProcess 冻结进程（曾导致全系统假死）
//   2. 绝不强制结束进程
//   3. 系统/前台/游戏本体/反作弊/音频/输入法/编译器/自己 = 永不触碰

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
/* winsock2 必须先于 windows.h，否则会因 winsock.h 冲突报重定义 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

#define MAX_NAMES 512
#define IDLE_INTERVAL_SEC       180   /* 冷模式轮询：3 分钟（期间每秒看 GPU） */

/* ---- 托盘 ---- */
#define IDI_APPICON            101
#define WM_TRAYICON            (WM_APP + 1)
#define IDM_SHOW               40001
#define IDM_TRIMNOW            40002
#define IDM_PAUSE              40003
#define IDM_EXIT               40004
#define IDM_DEFAULT_FAN        40005
#define IDM_FAN_BASE           40100   /* 40100 + level 就是具体档位（最高 40200）*/
#define IDM_FAN_AUTO           40250   /* 必须大于 IDM_FAN_BASE+100，否则 100% 会落在范围外 */
#define IDM_BG_ECO             40300   /* 后台进程降级 开关 */
#define IDM_MEMTOP             40301   /* 查看内存排行 */
#define IDM_LEAK_AUTO          40302   /* 内存泄漏自动处理 开关 */
#define IDM_MEMTOP_BASE        40400   /* 40400 + 序号 */
#define IDM_CLEAN              40006
#define IDM_CLEAN_C            40007
#define IDM_CLEAN_D            40008
#define IDM_OPEN_LOG           40009
#define IDM_CLEAN_BIN          40010

#define TIP_CLASS              L"MemTuneTipWnd"

/* 电源方案 GUID */
static const wchar_t* PWR_QUIET = L"381b4222-f694-41f0-9685-ff5bb260df2e"; /* 安静模式（自定义）*/
static const wchar_t* PWR_GAME  = L"8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"; /* 游戏模式 */

/* 风扇控制：FanBridge.exe 常驻托管 LibreHardwareMonitor，MemTune 经命名管道下指令。
   取代旧的 fanctl.ps1 + fan_target.txt + 计划任务方案。 */

/* 风扇档位（GPU 优先，读不到时回退 CPU） */
#define FAN_GAME     100   /* 游戏运行：全速 */
#define FAN_BUSY      55   /* CPU 占用 >75%：中高 */
#define FAN_NORMAL    35   /* CPU 占用 50-75%：中 */
#define FAN_IDLE      25   /* CPU 占用 <50%：静音 */
#define FAN_HYSTERESIS_SEC 15  /* 换档间隔下限（秒）：太短会发出呼呼的换档声 */
#define FAN_DEADBAND       10  /* 档位差小于这个百分比就维持不动 */

/* GPU >= 80%：风扇拉满，同时触发全量内存优化 */
#define GPU_HOT_PCT   80   /* GPU 门槛：风扇拉满 + 全量内存优化 */
#define HOT_CONFIRM    3   /* GPU 要连续 HOT_CONFIRM 次采样 ≥80% 才算真热 */
#define GPU_MID_PCT   50   /* GPU 50-80%：中速 */
#define GPU_MID_FAN   55
#define GPU_LOW_FAN   35   /* GPU <50% 时保底转速 */
#define GPU_SAMPLE_MS 20   /* GPU 采样间隔（PDH 需要两次采样求差）*/
#define GAME_INTERVAL_SEC        60   /* 热模式轮询：1 分钟（期间每 5 秒看 GPU 是否回落） */
#define GAME_TRIM_ABOVE_MB       32ULL  /* 热模式：32MB 以上全清 */
#define IDLE_TRIM_ABOVE_MB      120ULL  /* 托盘「立即优化」：只清 120MB 以上的大块头 */
#define GAME_PURGE_BELOW_GB     8.0
#define GAME_HARD_PURGE_GB      4.0   /* 热模式可用内存跌破 4GB：每轮都清缓存 */
#define GAME_TRIM_REPEAT         3     /* 热模式：一轮里连续 trim 3 遍 */
#define MIN_TRIM_SAVE_MB         4ULL  /* 清不到 4MB 的进程不值得反复折腾 */
#define STARTUP_GRACE_SEC         90   /* 开机 90 秒内不 trim：登录瞬间系统负载高，
                                          此时清工作集会让壁纸/输入法/资源管理器反复重载 */

// ==================== 硬保护（写死）====================
static const wchar_t* NEVER_TOUCH[] = {
    // 内核 / 关键服务
    L"system", L"idle", L"registry", L"memory compression", L"smss", L"csrss",
    L"wininit", L"winlogon", L"services", L"lsass", L"lsaiso", L"svchost",
    L"fontdrvhost", L"dwm", L"wudfhost", L"sihost", L"wmiprvse", L"spoolsv",
    L"dllhost", L"msdtc", L"trustedinstaller", L"tiworker",
    // 外壳 / 输入 / 音频
    L"explorer", L"conhost", L"taskhostw", L"runtimebroker", L"shellexperiencehost",
    L"startmenuexperiencehost", L"searchhost", L"searchapp", L"textinputhost",
    L"ctfmon", L"audiodg", L"audioendpointbuilder", L"audiosrv", L"lockapp",
    L"applicationframehost",
    // 输入法
    L"wetype", L"wetype_server", L"wetype_update", L"wetype_renderer", L"wechatappex",
    // 安全 / 驱动
    L"hipsdaemon", L"hipstray", L"sysdiag", L"usysdiag", L"hrsword",
    L"nvcontainer", L"nvdisplay.container", L"rtkauduservice", L"rtkuwp",
    // 壁纸引擎：渲染上下文被回收会自杀重启，显示「非正常关闭」
    L"wallpaper64", L"wallpaper32", L"wallpaperservice64", L"wallpaperservice32", L"ui32",
    // 开发 / 工作环境
    L"snipaste", L"cherry studio", L"cherrystudio", L"bash", L"node",
    L"python", L"pythonw", L"msedgewebview2", L"g++", L"gcc", L"cc1plus",
    L"cc1", L"as", L"ld", L"collect2", L"cpp", L"git",
    // 游戏本体 + 反作弊
    L"ace-tray", L"ace-service64", L"ace-helper", L"ace-setup64", L"anticheatexpert",
    L"sguard64", L"sguardsvc64", L"gameinputrawinputproxy", L"gameinputredistservice",
    L"steam", L"steamwebhelper", L"steamservice", L"steamclient",
    L"gameoverlayui", L"gameoverlayui64",
    L"deltaforceclient", L"deltaforceclient-win64-shipping", L"bhdclient",
    L"unrealcefsubprocess", L"javaw", L"java", L"minecraft", L"minecraft.windows",
    L"valorant-win64-shipping", L"cs2", L"dota2", L"pubg", L"tslgame", L"r5apex",
    // 自己
    L"powershell", L"wscript", L"cmd", L"memtune", L"msiexec",
    NULL
};

// ==================== 优先 trim（空闲也动）====================
static const wchar_t* FAT_BACKGROUND[] = {
    L"douyin", L"douyin_tray",
    /* Wallpaper Engine 不放这里：它的渲染上下文一旦被 EmptyWorkingSet 换出，
       恢复不及时会自行重启，表现为「上次被非正常关闭」。只进 NEVER_TOUCH。 */
    L"rainmeter",
    L"msedge", L"chrome", L"firefox", L"iexplore", L"ielowutil", L"crashpad_handler",
    L"searchindexer", L"searchprotocolhost", L"searchfilterhost",
    L"onedrive", L"dropbox", L"nvidia broadcast",
    L"wmplayer", L"spotify", L"qqmusic", L"cloudmusic", L"kugou",
    L"wechat", L"weixin", L"disco", L"discord", L"qq", L"tim",
    L"dingtalk", L"wemeetapp", L"wxwork",
    L"wegame", L"battle.net", L"epicgameslauncher", L"galaxyclient",
    NULL
};

// ==================== 内存占用排行（类型定义）====================
#define MEMTOP_MAX 20

typedef struct {
    wchar_t name[64];
    unsigned long long wsMB;
} MemTopItem;

// ==================== 全局 ====================
static wchar_t g_root[MAX_PATH];
static wchar_t g_games[MAX_NAMES][64];
static int     g_gameCount = 0;

// ==================== 工具 ====================
static void ToLowerW(wchar_t* s) {
    for (; *s; ++s) *s = towlower(*s);
}

static void StripExe(wchar_t* s) {
    size_t n = wcslen(s);
    if (n > 4 && _wcsicmp(s + n - 4, L".exe") == 0) s[n - 4] = 0;
}

static int InList(const wchar_t* name, const wchar_t** list) {
    for (int i = 0; list[i]; ++i) {
        if (_wcsicmp(name, list[i]) == 0) return 1;
    }
    return 0;
}

static void LogMsg(const wchar_t* fmt, ...) {
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 2047, fmt, ap);
    va_end(ap);
    msg[2047] = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t path[MAX_PATH + 32];
    _snwprintf(path, MAX_PATH + 31, L"%s\\memtune.log", g_root);
    path[MAX_PATH + 31] = 0;

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    // 写 UTF-8
    wchar_t line[2300];
    _snwprintf(line, 2299, L"%02d:%02d:%02d %s\r\n", st.wHour, st.wMinute, st.wSecond, msg);
    line[2299] = 0;

    char utf8[8100];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, 8099, NULL, NULL);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, utf8, (DWORD)(n - 1), &written, NULL);
    }
    CloseHandle(h);
}

static double GetFreeGB(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0.0;
    return (double)ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
}

// 清 standby 缓存。flags: 4=清 standby, 3=flush modified
static void PurgeSystem(int flags) {
    typedef LONG(WINAPI* PFN)(ULONG, PVOID, ULONG);
    static PFN pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) pfn = (PFN)GetProcAddress(ntdll, "NtSetSystemInformation");
    }
    if (!pfn) return;

    // 提权
    HANDLE hTok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok)) {
        const wchar_t* privs[] = { L"SeIncreaseQuotaPrivilege", L"SeProfileSingleProcessPrivilege" };
        for (int i = 0; i < 2; ++i) {
            LUID luid;
            if (LookupPrivilegeValueW(NULL, privs[i], &luid)) {
                TOKEN_PRIVILEGES tp;
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(hTok, FALSE, &tp, sizeof(tp), NULL, NULL);
            }
        }
        CloseHandle(hTok);
    }
    int v = flags;
    pfn(0x50, &v, sizeof(v));
}

/* ==================== GPU 占用率 ==================== */
/* 用内置 PDH 计数器读 GPU 引擎总占用（无外部依赖）。
   计数器：\GPU Engine(*)\Utilization Percentage
   把所有引擎实例的占用值相加并封顶 100。
   取不到（老驱动/无 GPU）时返回 -1，调用方回退到 CPU 判断。 */
static int GetGpuUsage(void)
{
    typedef PDH_STATUS(WINAPI* PFN_OPEN)(LPCSTR, DWORD_PTR, PDH_HQUERY*);
    typedef PDH_STATUS(WINAPI* PFN_ADD)(PDH_HQUERY, LPCSTR, DWORD_PTR, PDH_HCOUNTER*);
    typedef PDH_STATUS(WINAPI* PFN_COLLECT)(PDH_HQUERY);
    typedef PDH_STATUS(WINAPI* PFN_FMTARRAY)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD,
                                            PPDH_FMT_COUNTERVALUE_ITEM_A);
    typedef PDH_STATUS(WINAPI* PFN_CLOSE)(PDH_HQUERY);

    static HMODULE pdh = NULL;
    static PFN_OPEN     pOpen    = NULL;
    static PFN_ADD      pAdd     = NULL;
    static PFN_ADD      pAddLoc  = NULL;
    static PFN_COLLECT  pCollect = NULL;
    static PFN_FMTARRAY pFmtArr  = NULL;
    static PFN_CLOSE    pClose   = NULL;
    static int tried = 0;

    static PDH_HQUERY   hQuery   = NULL;
    static PDH_HCOUNTER hCounter = NULL;
    static int  hValid  = 0;
    static int  hSample = 0;
    static int  hFail   = 0;

    if (!tried) {
        tried = 1;
        pdh = LoadLibraryW(L"pdh.dll");
        if (pdh) {
            pOpen    = (PFN_OPEN)    GetProcAddress(pdh, "PdhOpenQuery");
            pAdd     = (PFN_ADD)     GetProcAddress(pdh, "PdhAddEnglishCounterA");
            pAddLoc  = (PFN_ADD)     GetProcAddress(pdh, "PdhAddCounterA");
            pCollect = (PFN_COLLECT) GetProcAddress(pdh, "PdhCollectQueryData");
            pFmtArr  = (PFN_FMTARRAY)GetProcAddress(pdh, "PdhGetFormattedCounterArrayA");
            pClose   = (PFN_CLOSE)   GetProcAddress(pdh, "PdhCloseQuery");
        }
        if (!pOpen || !pCollect || !pFmtArr || (!pAdd && !pAddLoc)) return -1;
    }

    /* 连续失败太多次就放弃，别每轮都折腾 */
    if (hFail >= 3) return -1;

    if (!hValid) {
        if (pOpen(NULL, 0, &hQuery) != ERROR_SUCCESS) { hQuery = NULL; hFail++; return -1; }

        PDH_STATUS r = PDH_CSTATUS_NO_OBJECT;
        /* 先走英文计数器（不受系统语言影响），失败再用本地化 */
        if (pAdd) r = pAdd(hQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &hCounter);
        if (r != ERROR_SUCCESS && pAddLoc)
            r = pAddLoc(hQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &hCounter);

        if (r != ERROR_SUCCESS) {
            if (pClose) pClose(hQuery);
            hQuery = NULL;
            hFail++;
            return -1;
        }
        hValid = 1;
        hSample = 0;
    }

    if (pCollect(hQuery) != ERROR_SUCCESS) { hFail++; return -1; }
    hSample++;
    if (hSample < 2) return -1;   /* 首次只建立基线 */

    DWORD sz = 0, cnt = 0;
    PDH_STATUS r = pFmtArr(hCounter, PDH_FMT_DOUBLE, &sz, &cnt, NULL);
    if (r != PDH_MORE_DATA || sz == 0) { hFail++; return -1; }

    PPDH_FMT_COUNTERVALUE_ITEM_A items =
        (PPDH_FMT_COUNTERVALUE_ITEM_A)malloc(sz);
    if (!items) return -1;

    r = pFmtArr(hCounter, PDH_FMT_DOUBLE, &sz, &cnt, items);
    if (r != ERROR_SUCCESS) { free(items); hFail++; return -1; }

    double sum = 0.0;
    for (DWORD i = 0; i < cnt; ++i) {
        if (items[i].FmtValue.CStatus == 0)
            sum += items[i].FmtValue.doubleValue;
    }
    free(items);

    if (sum < 0)   sum = 0;
    if (sum > 100) sum = 100;
    hFail = 0;
    return (int)sum;
}

/* 让进程页优先被换出：-1 = 低于正常（内存最早被回收）
   PROCESS_INFORMATION_CLASS::ProcessPagePriority = 2 */
static void SetPagePriority(HANDLE h, int priority)
{
    typedef LONG(WINAPI* PFN)(HANDLE, int, LPVOID);
    static PFN pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32) pfn = (PFN)GetProcAddress(k32, "SetProcessInformation");
    }
    if (!pfn) return;
    pfn(h, 2, &priority);
}

/* 设置内存优先级：1 = 最低（Normal=5, VeryLow=1）
   PROCESS_INFORMATION_CLASS::ProcessMemoryPriority = 5 */
static void SetMemoryPriority(HANDLE h, ULONG priority)
{
    typedef LONG(WINAPI* PFN)(HANDLE, int, LPVOID, ULONG);
    static PFN pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32) pfn = (PFN)GetProcAddress(k32, "SetProcessInformation");
    }
    if (!pfn) return;
    /* ProcessMemoryPriority = 5 */
    pfn(h, 5, &priority, sizeof(priority));
}

/* EcoQoS：降低后台进程 CPU 占用（PROCESS_INFORMATION_CLASS::ProcessPowerThrottling = 4）*/
static void SetEco(HANDLE h, BOOL on) {
    typedef BOOL(WINAPI* PFN)(HANDLE, int, LPVOID, DWORD);
    static PFN pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32) pfn = (PFN)GetProcAddress(k32, "SetProcessInformation");
    }
    if (!pfn) return;
    struct { ULONG Version; ULONG ControlMask; ULONG StateMask; } s;
    s.Version = 1;
    s.ControlMask = 1;
    s.StateMask = on ? 1 : 0;
    pfn(h, 4, &s, sizeof(s));
}

static void LoadGames(void) {
    g_gameCount = 0;
    wchar_t path[MAX_PATH + 32];
    _snwprintf(path, MAX_PATH + 31, L"%s\\games.txt", g_root);
    path[MAX_PATH + 31] = 0;

    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;

    DWORD sz = GetFileSize(hf, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 64 * 1024) { CloseHandle(hf); return; }

    char* raw = (char*)malloc(sz + 1);
    if (!raw) { CloseHandle(hf); return; }
    DWORD got = 0;
    ReadFile(hf, raw, sz, &got, NULL);
    CloseHandle(hf);
    raw[got] = 0;

    char* p = raw;
    if (got >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;

    while (*p && g_gameCount < MAX_NAMES) {
        char* e = strpbrk(p, "\r\n");
        size_t len = e ? (size_t)(e - p) : strlen(p);

        // trim
        while (len > 0 && (p[0] == ' ' || p[0] == '\t')) { p++; len--; }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;

        if (len > 0 && p[0] != '#') {
            wchar_t w[64];
            int n = MultiByteToWideChar(CP_UTF8, 0, p, (int)len, w, 63);
            if (n > 0) {
                w[n] = 0;
                ToLowerW(w);
                StripExe(w);
                wcscpy(g_games[g_gameCount], w);
                g_gameCount++;
            }
        }
        if (!e) break;
        p = e + 1;
        while (*p == '\r' || *p == '\n') p++;
    }
    free(raw);
}

static int IsGameRunning(wchar_t* outName) {
    if (g_gameCount == 0) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t name[64];
            wcsncpy(name, pe.szExeFile, 63);
            name[63] = 0;
            ToLowerW(name);
            StripExe(name);
            for (int i = 0; i < g_gameCount; ++i) {
                if (wcscmp(name, g_games[i]) == 0) {
                    if (outName) wcscpy(outName, name);
                    found = 1;
                    break;
                }
            }
            if (found) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

/* ==================== 自动游戏识别 ==================== */
/* 判断依据（任一满足即可）：
   1. 进程名在 games.txt 里（显式名单，最高优先）
   2. 进程名匹配已知游戏特征（Shipping / Client / Game / Launcher 等）
   3. 前台窗口是全屏独占（尺寸 == 屏幕尺寸，且位置 0,0）
   4. 进程显存占用 > 800MB 且 CPU 占用 > 15%
*/

/* 已知游戏进程名特征 */
static int LooksLikeGameProcess(const wchar_t* name)
{
    static const wchar_t* pats[] = {
        L"-win64-shipping", L"-win32-shipping", L"win64-shipping",
        L"client-win64", L"game-win64", L"-vgui",
        L"deltaforce", L"battlefield", L"callofduty", L"codmw",
        L"eldenring", L"sekiro", L"armoredcore",
        L"cyberpunk", L"witcher", L"gta5", L"gta6", L"rdr2",
        L"valorant", L"leagueoflegends", L"dota2", L"cs2", L"csgo",
        L"pubg", L"tslgame", L"r5apex", L"apex_legends",
        L"minecraft", L"javaw", L"genshin", L"yuanshen",
        L"starrail", L"wutheringwaves", L"zenlesszonezero",
        L"farcry", L"assassinscreed", L"hitman", L"control",
        L"residentevil", L"finalfantasy", L"persona",
        L"monster hunter", L"mhrise", L"mhwilds",
        NULL
    };
    for (int i = 0; pats[i]; ++i) {
        if (wcsstr(name, pats[i])) return 1;
    }
    return 0;
}

/* 前台窗口是否全屏独占 */
static int IsForegroundFullscreen(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;

    /* 桌面/任务栏不算 */
    HWND desk = GetDesktopWindow();
    HWND shell = GetShellWindow();
    if (fg == desk || fg == shell) return 0;

    RECT rc;
    if (!GetWindowRect(fg, &rc)) return 0;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return 0;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    /* 全屏：覆盖整个屏幕且从 0,0 开始 */
    if (!(rc.left <= 0 && rc.top <= 0 && w >= sw && h >= sh)) return 0;

    /* 进一步：全屏独占窗口通常没有标题栏/边框。
       有 WS_CAPTION 的是普通最大化窗口，不算全屏游戏。 */
    LONG style = GetWindowLongW(fg, GWL_STYLE);
    if (style & WS_CAPTION) {
        /* 有标题栏：只有当它同时没有 WS_THICKFRAME 且最大化时才可能是游戏 */
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            /* 最大化窗口会正好等于工作区，不是全屏 */
            if (w <= (wa.right - wa.left) && h <= (wa.bottom - wa.top)) return 0;
        }
        return 0;
    }
    return 1;
}

/* ==================== D3DKMT 独占全屏检测 ==================== */
/* 微软官方 API，判断前台进程是否独占显示器。
   返回 OwnerType: 0=无 1=共享 2=独占 3=独占GDI 4=模拟独占 */
#define D3DKMT_VIDPN_EXCLUSIVE_NOT_OWNED        0
#define D3DKMT_VIDPN_EXCLUSIVE_SHARED           1
#define D3DKMT_VIDPN_EXCLUSIVE_EXCLUSIVE        2
#define D3DKMT_VIDPN_EXCLUSIVE_EXCLUSIVE_GDI    3
#define D3DKMT_VIDPN_EXCLUSIVE_EMULATED         4

typedef struct {
    UINT  hAdapter;
    UINT  VidPnSourceId;
    INT   OwnerType;
} D3DKMT_QUERYVIDPNEXCLUSIVEOWNERSHIP;

typedef LONG NTSTATUS;
typedef NTSTATUS (WINAPI *PFN_D3DKMTQueryVidPnExclusiveOwnership)(D3DKMT_QUERYVIDPNEXCLUSIVEOWNERSHIP*);

/* 检查前台是否处于全屏独占或模拟独占（= 游戏） */
static int IsExclusiveFullscreenGame(void)
{
    static PFN_D3DKMTQueryVidPnExclusiveOwnership pfnQuery = NULL;
    static int tried = 0;

    if (!tried) {
        tried = 1;
        HMODULE gdi = LoadLibraryW(L"gdi32.dll");
        if (gdi) {
            /* D3DKMTQueryVidPnExclusiveOwnership 在 Win8+ 的 gdi32 里 */
            pfnQuery = (PFN_D3DKMTQueryVidPnExclusiveOwnership)
                       GetProcAddress(gdi, "D3DKMTQueryVidPnExclusiveOwnership");
        }
    }
    if (!pfnQuery) return 0;

    /* 遍历主显示源 0 和 1（多显示器） */
    for (UINT src = 0; src < 2; ++src) {
        D3DKMT_QUERYVIDPNEXCLUSIVEOWNERSHIP q;
        ZeroMemory(&q, sizeof(q));
        q.hAdapter = 0;
        q.VidPnSourceId = src;

        if (pfnQuery(&q) == 0) {
            if (q.OwnerType == D3DKMT_VIDPN_EXCLUSIVE_EXCLUSIVE ||
                q.OwnerType == D3DKMT_VIDPN_EXCLUSIVE_EXCLUSIVE_GDI ||
                q.OwnerType == D3DKMT_VIDPN_EXCLUSIVE_EMULATED) {
                return 1;
            }
        }
    }
    return 0;
}

/* 前台进程有多少显存占用（用 PDH 计数器，简单版） */
/* 检测前台进程加载的模块，判断是否为游戏。
   依据来源：Valve SteamDatabase FileDetectionRuleSets (rules.ini)
   + 各引擎官方文档的运行时 DLL 特征。

   三层判断：
     A. 图形 API（必须）—— d3d11/d3d12/vulkan 等
     B. 游戏引擎特征 DLL（强信号）
     C. 游戏运行时库（音频/物理/DRM，强信号）

   返回：0=不是游戏  1=疑似游戏  2=高度确信是游戏 */
static int ForegroundUsesGpu(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid <= 4) return 0;

    /* ---- A. 图形 API ---- */
    static const wchar_t* gfxDlls[] = {
        L"d3d11.dll", L"d3d12.dll", L"d3d9.dll",
        L"vulkan-1.dll", L"opengl32.dll",
        L"nvoglv64.dll", L"nvoglv32.dll",
        L"atidxx64.dll", L"atio6axx.dll",
        L"igxelpc64.dll", L"igd12umd64.dll",
        NULL
    };

    /* ---- B. 游戏引擎特征（来自 Valve rules.ini）---- */
    static const wchar_t* engineDlls[] = {
        /* Unity */
        L"unityplayer.dll",
        /* Unreal (UE4/UE5 靠 exe 名 + Shipping 后缀，这里补运行时) */
        L"ue4game.exe", L"ue5game.exe", L"unrealcefsubprocess.exe",
        /* CryEngine */
        L"cry3dengine.dll", L"cryrenderd3d11.dll", L"cryrenderd3d12.dll",
        L"cryrendervulkan.dll", L"cryd3dcompilerstub.dll",
        /* Frostbite */
        L"engine.buildinfo.dll", L"runtime_win64_retail.buildsettings",
        /* Godot */
        L"godotsharp.dll", L"godotsteam.dll",
        /* idTech */
        L"idtech.dll",
        /* Cocos */
        L"libcocos2d.dll", L"cocos2d.dll",
        NULL
    };

    /* ---- C. 游戏运行时（音频/物理/DRM/反作弊）---- */
    static const wchar_t* runtimeDlls[] = {
        /* 物理 */
        L"physx3_x64.dll", L"physx3common_x64.dll", L"physx4_x64.dll",
        L"physxgpu64.dll", L"physxcooking64.dll",
        /* 音频中间件（游戏特有） */
        L"fmod64.dll", L"fmod.dll", L"fmodstudio64.dll",
        L"wwise64.dll", L"akaudio.dll",
        L"cri_ware_unity.dll", L"criware.dll",
        /* 视频解码（游戏过场） */
        L"bink2w64.dll", L"binkw64.dll", L"bink2w32.dll",
        /* DRM / 平台 */
        L"steam_api64.dll", L"steam_api.dll",
        L"galaxy64.dll", L"eossdk-win64-shipping.dll",
        /* 反作弊（游戏独有） */
        L"easyanticheat.dll", L"beservice.exe", L"ace-base.dll",
        L"battleye.dll", L"xgameruntime.dll",
        /* 图形增强 */
        L"nvngx_dlss.dll", L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_fsr.dll",
        /* Steam 覆盖层（只在游戏中注入） */
        L"gameoverlayrenderer64.dll", L"gameoverlayrenderer.dll",
        NULL
    };

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    int hasGfx = 0, hasEngine = 0, hasRuntime = 0;

    if (Module32FirstW(hSnap, &me)) {
        do {
            wchar_t low[128];
            wcsncpy(low, me.szModule, 127);
            low[127] = 0;
            ToLowerW(low);

            if (!hasGfx) {
                for (int i = 0; gfxDlls[i]; ++i)
                    if (wcscmp(low, gfxDlls[i]) == 0) { hasGfx = 1; break; }
            }
            if (!hasEngine) {
                for (int i = 0; engineDlls[i]; ++i)
                    if (wcscmp(low, engineDlls[i]) == 0) { hasEngine = 1; break; }
            }
            if (!hasRuntime) {
                for (int i = 0; runtimeDlls[i]; ++i)
                    if (wcscmp(low, runtimeDlls[i]) == 0) { hasRuntime = 1; break; }
            }
            if (hasGfx && (hasEngine || hasRuntime)) break;
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);

    /* 判定：
       - 图形API + 引擎特征  -> 高度确信 (2)
       - 图形API + 游戏运行时 -> 高度确信 (2)
       - 仅有图形API          -> 疑似 (1)，还需其他条件配合
    */
    if (hasGfx && (hasEngine || hasRuntime)) return 2;
    if (hasGfx) return 1;
    return 0;
}

/* 非游戏程序排除名单 */
static int IsNotGameName(const wchar_t* name)
{
    static const wchar_t* notGame[] = {
        /* browsers / players */
        L"chrome", L"msedge", L"firefox", L"iexplore", L"opera", L"brave",
        L"vlc", L"mpc-hc64", L"mpc-hc", L"potplayermini64", L"potplayermini",
        L"bilibili", L"iqiyi", L"youku", L"qqplayer", L"kmplayer",
        /* desktop / tools */
        L"explorer", L"cherry studio", L"cherrystudio",
        L"wetype", L"wetype_renderer", L"snipaste",
        L"fancontrol", L"fancontrol.service",
        L"wpp", L"powerpnt", L"excel", L"winword", L"wps",
        L"code", L"devenv", L"pycharm64", L"idea64", L"goland64",
        L"mstsc", L"teamviewer", L"anydesk", L"todesk",
        L"obs64", L"obs32", L"obs",
        L"rainmeter", L"wallpaper64", L"wallpaper32", L"wallpaperservice64",
        /* system shell */
        L"applicationframehost", L"shellexperiencehost", L"lockapp",
        L"textinputhost", L"searchhost", L"startmenuexperiencehost",
        L"dwm", L"winlogon", L"logonui", L"taskmgr", L"mmc", L"consent",
        /* installers / updaters */
        L"msiexec", L"setup", L"install", L"update", L"uninstall",
        /* emulators & dev */
        L"mumuplayer", L"nemuplayer", L"dnplayer", L"bluestacks",
        L"steam", L"steamwebhelper", L"epicgameslauncher", L"wegame",
        NULL
    };
    for (int i = 0; notGame[i]; ++i) {
        if (wcscmp(name, notGame[i]) == 0) return 1;
    }
    return 0;
}

/* 综合判断：返回 1=游戏中 */
/* 前置声明：定义在下方（TrimProcsMulti 在文件后半段）*/
typedef struct {
    int trimmed;
    unsigned long long freedMB;
    int skipped;
} TrimResult;

static TrimResult TrimProcsMulti(unsigned long long minAboveMB, int deep, int maxPass);

/* ---- 通用游戏启动预优化 ----
   任何 games.txt 里的游戏（不只是三角洲）一出现，立即腾内存。
   分两阶段，避免干扰加载：
     A（发现当轮）：清 standby 缓存 + 只清 256MB 以上大块头 —— 轻、快
     B（20 秒后）：加载高峰过去，深度 trim 一次
   状态按进程名独立记，切游戏不会互相干扰。 */
#define PREOPT_MAX_GAMES  8

typedef struct {
    wchar_t name[64];
    int     stage;      /* 0=未出现 1=已做A 2=已做B */
    DWORD   firstTick;
} PreOptState;

static PreOptState g_preopt[PREOPT_MAX_GAMES];

static PreOptState* PreOptSlot(const wchar_t* name)
{
    /* 同名槽位复用 */
    for (int i = 0; i < PREOPT_MAX_GAMES; ++i)
        if (g_preopt[i].stage && wcscmp(g_preopt[i].name, name) == 0)
            return &g_preopt[i];
    /* 找空位 */
    for (int i = 0; i < PREOPT_MAX_GAMES; ++i)
        if (g_preopt[i].stage == 0) {
            wcsncpy(g_preopt[i].name, name, 63);
            g_preopt[i].name[63] = 0;
            return &g_preopt[i];
        }
    return &g_preopt[0];   /* 槽位满了就覆盖第一个 */
}

/* 每轮调用一次：传入当前在跑的游戏名（无游戏传 NULL）*/
static void GamePreOptTick(const wchar_t* runningGame)
{
    if (!runningGame || !runningGame[0]) return;

    PreOptState* s = PreOptSlot(runningGame);
    if (s->stage == 0) {
        s->stage = 1;
        s->firstTick = GetTickCount();

        /* 阶段 A：立即做，最轻的一档 */
        double before = GetFreeGB();
        PurgeSystem(4);                                   /* 清 standby，不触脏页 */
        TrimResult r = TrimProcsMulti(256ULL, 0, 1);      /* 只清 256MB 以上大块头 */
        double after = GetFreeGB();
        LogMsg(L"preopt A [%s]: trim=%d freed=%lluMB free=%.2f->%.2fGB",
               runningGame, r.trimmed, r.freedMB, before, after);
        return;
    }

    if (s->stage == 1 && (GetTickCount() - s->firstTick) > 20000) {
        s->stage = 2;
        double before = GetFreeGB();
        TrimResult r = TrimProcsMulti(IDLE_TRIM_ABOVE_MB, 0, 2);
        double after = GetFreeGB();
        LogMsg(L"preopt B [%s]: trim=%d freed=%lluMB free=%.2f->%.2fGB",
               runningGame, r.trimmed, r.freedMB, before, after);
    }
}

/* 游戏退出后清掉它的状态，下次启动能重新做 */
static void GamePreOptReset(const wchar_t* name)
{
    for (int i = 0; i < PREOPT_MAX_GAMES; ++i)
        if (g_preopt[i].stage && wcscmp(g_preopt[i].name, name) == 0)
            g_preopt[i].stage = 0;
}

static int DetectGameAuto(wchar_t* outName)
{
    /* 优先级 1：games.txt 显式名单 */
    if (g_gameCount > 0) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    wchar_t name[64];
                    wcsncpy(name, pe.szExeFile, 63);
                    name[63] = 0;
                    ToLowerW(name);
                    StripExe(name);
                    for (int i = 0; i < g_gameCount; ++i) {
                        if (wcscmp(name, g_games[i]) == 0) {
                            if (outName) wcscpy(outName, name);
                            CloseHandle(snap);
                            return 1;
                        }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    /* 优先级 2：进程名特征 */
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    wchar_t name[64];
                    wcsncpy(name, pe.szExeFile, 63);
                    name[63] = 0;
                    ToLowerW(name);
                    StripExe(name);
                    if (LooksLikeGameProcess(name)) {
                        if (outName) wcscpy(outName, name);
                        CloseHandle(snap);
                        return 1;
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    /* 优先级 3a：D3DKMT 独占全屏（最准，游戏专用） */
    if (IsExclusiveFullscreenGame()) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE && pid > 4) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (pe.th32ProcessID == pid) {
                        wchar_t nm[64];
                        wcsncpy(nm, pe.szExeFile, 63);
                        nm[63] = 0;
                        ToLowerW(nm);
                        StripExe(nm);
                        if (IsNotGameName(nm)) { CloseHandle(snap); goto layer3b; }
                        if (outName) wcscpy(outName, nm);
                        CloseHandle(snap);
                        return 1;
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

layer3b:
    /* 优先级 3b：模块特征检测（引擎/运行时/图形API）
       - score==2（引擎或游戏运行时特征）：直接判定，不需要全屏
       - score==1（仅图形API）：需要全屏 + 工作集>300MB
       - 一律排除非游戏名单 */
    {
        int score = ForegroundUsesGpu();
        int ok = (score == 2);
        if (score == 1 && IsForegroundFullscreen()) ok = 1;
        if (!ok) goto done_layer3;
    }
    if (1) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid > 4) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe;
                pe.dwSize = sizeof(pe);
                if (Process32FirstW(snap, &pe)) {
                    do {
                        if (pe.th32ProcessID == pid) {
                            wchar_t name[64];
                            wcsncpy(name, pe.szExeFile, 63);
                            name[63] = 0;
                            ToLowerW(name);
                            StripExe(name);
                            if (!IsNotGameName(name)) {
                                /* 工作集门槛：游戏通常 > 300MB */
                                HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                                        FALSE, pid);
                                int bigEnough = 0;
                                if (hp) {
                                    PROCESS_MEMORY_COUNTERS pmc;
                                    pmc.cb = sizeof(pmc);
                                    if (GetProcessMemoryInfo(hp, &pmc, sizeof(pmc))) {
                                        if (pmc.WorkingSetSize > (300ULL * 1024 * 1024))
                                            bigEnough = 1;
                                    }
                                    CloseHandle(hp);
                                }
                                if (bigEnough) {
                                    if (outName) wcscpy(outName, name);
                                    CloseHandle(snap);
                                    return 1;
                                }
                            }
                        }
                    } while (Process32NextW(snap, &pe));
                }
                CloseHandle(snap);
            }
        }
    }

done_layer3:
    return 0;
}

static DWORD GetFgPid(void) {
    HWND h = GetForegroundWindow();
    if (!h) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

static TrimResult TrimProcs(unsigned long long minAboveMB, int deep) {
    TrimResult r = { 0, 0, 0 };
    DWORD fgPid = GetFgPid();
    DWORD myPid = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return r;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return r; }

    do {
        DWORD pid = pe.th32ProcessID;
        if (pid <= 4 || pid == myPid || pid == fgPid) continue;

        wchar_t name[64];
        wcsncpy(name, pe.szExeFile, 63);
        name[63] = 0;
        ToLowerW(name);
        StripExe(name);

        if (InList(name, NEVER_TOUCH)) continue;
        if (!deep && !InList(name, FAT_BACKGROUND)) { r.skipped++; continue; }

        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA | PROCESS_SET_INFORMATION,
                               FALSE, pid);
        if (!h) { r.skipped++; continue; }

        PROCESS_MEMORY_COUNTERS_EX pmc;
        pmc.cb = sizeof(pmc);
        if (!GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            CloseHandle(h); r.skipped++; continue;
        }

        unsigned long long wsMB = pmc.WorkingSetSize / (1024ULL * 1024ULL);
        if (wsMB < minAboveMB) { CloseHandle(h); continue; }

        SIZE_T before = pmc.WorkingSetSize;

        /* === 核心 ===
           1. 内存优先级压到最低 -> 系统换页时优先挑它
           2. 清工作集 -> 活跃页也一起换出（不止 inactive）
           3. 再压优先级 -> 防止刚换出就抢回来 */
        SetMemoryPriority(h, 1);          /* VeryLow */
        SetPagePriority(h, -1);           /* 页面换出优先级最低 */
        EmptyWorkingSet(h);
        SetMemoryPriority(h, 1);

        if (deep) {
            SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
            SetEco(h, TRUE);
        }

        PROCESS_MEMORY_COUNTERS_EX pmc2;
        pmc2.cb = sizeof(pmc2);
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc2, sizeof(pmc2))) {
            if (pmc2.WorkingSetSize < before)
                r.freedMB += (unsigned long long)((before - pmc2.WorkingSetSize) / (1024ULL * 1024ULL));
        }
        CloseHandle(h);
        r.trimmed++;
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return r;
}

/* 多遍 trim：反复扫，把刚被唤醒又抢回内存的进程再压下去
   直到某遍回收量 < MIN_TRIM_SAVE_MB 或达到 maxPass 遍 */
static TrimResult TrimProcsMulti(unsigned long long minAboveMB, int deep, int maxPass)
{
    TrimResult total = { 0, 0, 0 };
    for (int pass = 0; pass < maxPass; ++pass) {
        TrimResult r = TrimProcs(minAboveMB, deep);
        total.trimmed += r.trimmed;
        total.freedMB += r.freedMB;
        total.skipped  = r.skipped;
        if (pass > 0 && r.freedMB < MIN_TRIM_SAVE_MB) break;
        if (pass + 1 < maxPass) Sleep(120);   /* 给系统一点时间落页 */
    }
    return total;
}

/* ==================== 电源方案切换 ==================== */
/* 调用 powercfg 切换方案。用 -setactive 需要管理员权限；
   若失败则静默忽略（不影响内存优化主功能） */
static int g_powerState = -1;   /* -1=未知 0=安静 1=游戏 */

static void ApplyPowerScheme(const wchar_t* guid, int state, int force)
{
    if (!force && g_powerState == state) return;   /* 已经是该方案 */

    wchar_t cmd[512];
    _snwprintf(cmd, 511,
        L"powercfg.exe -setactive %s", guid);
    cmd[511] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (ok) {
        WaitForSingleObject(pi.hProcess, 5000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (code == 0) {
            g_powerState = state;
            LogMsg(L"power -> %s", (state == 1) ? L"游戏模式(睿频开)" : L"安静模式(睿频关)");
        } else {
            LogMsg(L"power switch FAILED (code=%lu) — 需要管理员权限", code);
        }
    } else {
        LogMsg(L"power switch FAILED (CreateProcess err=%lu)", GetLastError());
    }
}

/* ==================== 风扇控制 ==================== */
static int  g_fanLevel      = -1;      /* 当前风扇档位百分比 */
static DWORD g_fanLastChange = 0;      /* 上次切换时刻 */
static volatile int g_fanManualOff = 0; /* 用户手动交还 BIOS，自动调速暂停 */
static volatile int g_bgEco        = 0; /* 后台进程 EcoQoS 降级：默认关（改动太全局）*/
static volatile int g_showingMemTop = 0; /* 正在展开内存排行时暂停刷新 */

/* 内存排行快照：菜单项编号只能传整数，具体进程名靠这张表回溯 */
static MemTopItem g_memTopSnap[MEMTOP_MAX];
static int        g_memTopSnapN = 0;

/* ---- 托盘状态 ---- */
static HWND   g_trayHwnd   = NULL;
static HICON  g_icon       = NULL;
static volatile int g_quit       = 0;   /* 托盘「退出」置位 */
static volatile int g_paused     = 0;   /* 托盘「暂停优化」置位 */
static volatile int g_trimNow    = 0;   /* 托盘「立即优化」置位 */
/* 供托盘窗口显示的实时数据 */
static volatile int g_lastGpu   = -1;
static volatile int g_lastCpu   = -1;
static volatile double g_lastFreeGB = 0.0;
static volatile int g_lastMode  = 0;    /* 0=idle 1=hot 2=paused */
static volatile int g_lastDfInGame = 0; /* 三角洲局内？1=是 */
static volatile int g_lastDfRun    = 0; /* 三角洲进程在跑？ */
static volatile int g_lastDfCpu    = 0; /* 三角洲 CPU 占用 */
static wchar_t g_lastGame[64] = L"";

/* 取 CPU 总占用率（0-100） */
static int GetCpuUsage(void)
{
    static ULARGE_INTEGER s_prevIdle, s_prevKernel, s_prevUser;
    static int s_inited = 0;

    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0;

    ULARGE_INTEGER i, k, u;
    i.LowPart = idle.dwLowDateTime;      i.HighPart = idle.dwHighDateTime;
    k.LowPart = kernel.dwLowDateTime;    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;      u.HighPart = user.dwHighDateTime;

    if (!s_inited) {
        s_prevIdle = i; s_prevKernel = k; s_prevUser = u;
        s_inited = 1;
        return 0;
    }

    ULONGLONG dIdle = i.QuadPart - s_prevIdle.QuadPart;
    ULONGLONG dKernel = k.QuadPart - s_prevKernel.QuadPart;
    ULONGLONG dUser = u.QuadPart - s_prevUser.QuadPart;

    s_prevIdle = i; s_prevKernel = k; s_prevUser = u;

    ULONGLONG total = dKernel + dUser;   /* kernel 已含 idle */
    if (total == 0) return 0;

    double busy = (double)(total - dIdle) / (double)total * 100.0;
    if (busy < 0) busy = 0;
    if (busy > 100) busy = 100;
    return (int)busy;
}

/* ==================== 风扇控制（FanBridge 常驻 + 命名管道）==================== */
/* FanBridge.exe 常驻，进程内托管 LibreHardwareMonitor 并持有 SuperIO 句柄。
   MemTune 用命名管道发指令，避免每轮重启 LHM（初始化数百毫秒，且 LpcIO 单持有）。 */
#define FAN_PIPE_NAME    L"\\\\.\\pipe\\MemTuneFanCtl"
#define FAN_BRIDGE_EXE   L"FanBridge.exe"

static HANDLE g_fanPipe = INVALID_HANDLE_VALUE;

/* 连到 FanBridge；首次自动拉起（ShellExecute runas 提权，端口 I/O 必需） */
static int FanConnect(int spawn)
{
    if (g_fanPipe != INVALID_HANDLE_VALUE) return 1;

    int tries = spawn ? 40 : 1;
    for (int attempt = 0; attempt < tries; ++attempt) {
        g_fanPipe = CreateFileW(FAN_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                                0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_fanPipe != INVALID_HANDLE_VALUE) return 1;

        if (spawn && attempt == 0) {
            wchar_t exe[MAX_PATH + 32];
            _snwprintf(exe, MAX_PATH + 31, L"%s\\%s", g_root, FAN_BRIDGE_EXE);
            exe[MAX_PATH + 31] = 0;
            HINSTANCE r = ShellExecuteW(NULL, L"runas", exe, NULL, g_root, SW_HIDE);
            if ((INT_PTR)r <= 32) {
                LogMsg(L"fan: bridge spawn failed (%ld)", (long)(INT_PTR)r);
                return 0;
            }
            LogMsg(L"fan: bridge spawned, waiting");
        }
        Sleep(200);
    }
    return 0;
}

/* 发一条指令，读回一行（最多 outSize-1 字节） */
/* 把管道设成超时模式，防止 ReadFile 永久阻塞把主线程卡死 */
static void FanSetPipeTimeouts(void)
{
    if (g_fanPipe == INVALID_HANDLE_VALUE) return;
    COMMTIMEOUTS ct;
    memset(&ct, 0, sizeof(ct));
    ct.ReadIntervalTimeout        = 300;
    ct.ReadTotalTimeoutMultiplier = 0;
    ct.ReadTotalTimeoutConstant   = 1000;   /* 最多等 1 秒 */
    ct.WriteTotalTimeoutConstant  = 1000;
    SetCommTimeouts(g_fanPipe, &ct);
}

/* 发送一条命令。
   重要：无论调用者要不要响应，都必须把回包读掉，
   否则残留的 OK 会导致下一条命令读到上一次的回复，最终堵死管道。*/
static int FanCommand(const char* cmd, char* out, int outSize)
{
    if (!FanConnect(1)) return 0;
    FanSetPipeTimeouts();

    DWORD written = 0;
    if (!WriteFile(g_fanPipe, cmd, (DWORD)strlen(cmd), &written, NULL)) {
        CloseHandle(g_fanPipe);
        g_fanPipe = INVALID_HANDLE_VALUE;
        return 0;
    }

    /* 不管 out 是否为空，都把这一行回包读走 */
    char tmp[256];
    DWORD got = 0, total = 0;
    while (total < (DWORD)sizeof(tmp) - 1) {
        char c = 0;
        if (!ReadFile(g_fanPipe, &c, 1, &got, NULL) || got == 0) break;
        if (c == '\n') break;
        tmp[total++] = c;
    }
    tmp[total] = 0;

    if (out && outSize > 1) {
        int n = (int)total;
        if (n > outSize - 1) n = outSize - 1;
        memcpy(out, tmp, (size_t)n);
        out[n] = 0;
    }
    return 1;
}

/* 设置目标风扇百分比 */
/* 真正下发档位给 FanBridge。手动/自动都走这里。 */
static void FanSend(int level)
{
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    g_fanLevel = level;

    char cmd[32], resp[128];
    _snprintf(cmd, 31, "SET %d\n", level);
    cmd[31] = 0;

    if (FanCommand(cmd, resp, (int)sizeof(resp))) {
        LogMsg(L"fan target -> %d%%", level);
    } else {
        /* 桥接不可用：退回文件模式（fanctl.ps1 若在跑仍响应） */
        wchar_t path[MAX_PATH + 32];
        _snwprintf(path, MAX_PATH + 31, L"%s\\fan_target.txt", g_root);
        path[MAX_PATH + 31] = 0;
        char buf[16];
        _snprintf(buf, 15, "%d\r\n", level);
        buf[15] = 0;
        HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL);
            CloseHandle(h);
        }
        LogMsg(L"fan target -> %d%% (file fallback)", level);
    }
}

static void ApplyFan(int level, int force)
{
    /* 手动模式下自动调速完全停手，由托盘菜单直接调 FanSend */
    if (g_fanManualOff) return;

    DWORD now = GetTickCount();
    if (!force) {
        if (g_fanLevel == level) return;
        {
            int diff = level - g_fanLevel;
            if (diff < 0) diff = -diff;
            if (g_fanLevel >= 0 && diff < FAN_DEADBAND) return;   /* 差得不够多：不值得降/升一档 */
        }
        if (g_fanLastChange != 0 && (now - g_fanLastChange) < (FAN_HYSTERESIS_SEC * 1000))
            return;
    }
    g_fanLastChange = now;
    FanSend(level);
}
/* 负载读数指数平滑（alpha≈0.25）。原始 GPU 读数在 20%~27% 之间来回抖，
   直接判定会让风扇每 5~6 秒换一次档，听感是持续的呼呼声；
   平滑后再判定，档位才稳定。raw<0 表示读取失败，原样返回。*/
static int s_gpuSm = -1, s_cpuSm = -1;
static int SmoothLoad(int* st, int raw)
{
    if (raw < 0) return raw;
    if (*st < 0) *st = raw * 16;
    *st += (((raw * 16) - *st) / 4);
    return (*st + 8) / 16;
}

/* 根据 GPU 占用率决定风扇档位（GPU 优先，取不到时回退 CPU）
   gpuPct < 0 表示读取失败 */
static int DecideFanByLoad(int gpuPct, int cpuPct, int inGame)
{
    if (gpuPct >= 0) {
        if (gpuPct >= GPU_HOT_PCT) return FAN_GAME;     /* GPU >=80%：拉满 */
        if (inGame)                return FAN_GAME;     /* 游戏内至少全速 */
        if (gpuPct >= GPU_MID_PCT) return GPU_MID_FAN;  /* 50-80%：55% */
        if (cpuPct >= 75)          return FAN_BUSY;     /* GPU 低但 CPU 忙：兜底 */
        if (gpuPct >= 20)          return GPU_LOW_FAN;  /* 20-50%：35% */
        return FAN_IDLE;                                 /* <20%：25% 静音 */
    }

    /* GPU 读不到：退回 CPU 策略 */
    if (inGame)          return FAN_GAME;
    if (cpuPct >= 75)    return FAN_BUSY;
    if (cpuPct >= 50)    return FAN_NORMAL;
    return FAN_IDLE;
}

/* ==================== 游戏时关闭程序 ==================== */
/* 规则：游戏启动时，关闭除下方 KEEP 名单外的所有用户级进程。
   游戏退出时只恢复 Snipaste（截屏工具），其他不恢复。 */

static const wchar_t* GAME_KEEP[] = {
    /* ---- Windows 内核 / 关键服务 ---- */
    L"system", L"idle", L"registry", L"memory compression", L"smss", L"csrss",
    L"wininit", L"winlogon", L"services", L"lsass", L"lsaiso", L"svchost",
    L"fontdrvhost", L"dwm", L"wudfhost", L"sihost", L"wmiprvse", L"spoolsv",
    L"dllhost", L"msdtc", L"trustedinstaller", L"tiworker", L"securityhealthservice",
    L"securityhealthsystray",

    /* ---- 桌面外壳 / 输入 / 音频（必须）---- */
    L"explorer", L"conhost", L"taskhostw", L"runtimebroker", L"shellexperiencehost",
    L"startmenuexperiencehost", L"searchhost", L"searchapp", L"textinputhost",
    L"ctfmon", L"audiodg", L"audioendpointbuilder", L"audiosrv", L"lockapp",
    L"applicationframehost", L"gamebar", L"gamebarftserver", L"gameinputsvc",

    /* ---- 输入法（打字必须）---- */
    L"wetype", L"wetype_server", L"wetype_update", L"wetype_renderer",

    /* ---- 安全 / 驱动 / 托盘（必须）---- */
    L"hipsdaemon", L"hipstray", L"sysdiag", L"usysdiag", L"hrsword",
    L"nvcontainer", L"nvdisplay.container", L"nvsphelper64",
    L"rtkauduservice", L"rtkauduservice64", L"rtkuwp",
    L"atieclxx", L"atiesrxx", L"amdrsserv", L"amdrssrcext", L"radeonsoftware",
    L"amdow", L"cncmd", L"amdfendrsr",

    /* ---- 抖音（用户明确要求保留）---- */
    L"douyin", L"douyin_tray", L"douyin_guard", L"douyin_browser",

    /* ---- 壁纸引擎（用户明确要求保留）---- */
    L"wallpaper64", L"wallpaper32", L"wallpaperservice64",
    L"wallpaperservice32", L"ui32",

    /* ---- 游戏本体 + 反作弊（绝对不能关）---- */
    L"ace-tray", L"ace-service64", L"ace-helper", L"ace-setup64",
    L"anticheatexpert", L"sguard64", L"sguardsvc64",
    L"deltaforceclient", L"deltaforceclient-win64-shipping", L"bhdclient",
    L"unrealcefsubprocess", L"gameinputrawinputproxy", L"gameinputredistservice",
    L"battleye", L"beservice", L"easyanticheat", L"easyanticheat_eos",
    L"start_protected_game",

    /* ---- Steam（三角洲是 Steam 游戏）---- */
    L"steam", L"steamwebhelper", L"steamservice", L"steamclient",
    L"gameoverlayui", L"gameoverlayui64",

    /* ---- 自己 / 系统工具 ---- */
    L"memtune", L"powershell", L"wscript", L"cmd", L"msiexec",
    L"taskmgr", L"logonui", L"consent", L"userinit",
    L"wudfhost", L"wudfrd", L"wmpnetwk", L"wmpnscfg",

    NULL
};

/* 游戏退出后需要恢复的程序 */
static const wchar_t* RESTORE_AFTER_GAME[] = {
    L"D:\\APP\\Snipaste_481166\\Snipaste.exe",
    NULL
};

static int IsGameKeep(const wchar_t* name)
{
    for (int i = 0; GAME_KEEP[i]; ++i) {
        if (_wcsicmp(name, GAME_KEEP[i]) == 0) return 1;
    }
    return 0;
}

/* 关闭所有不在 KEEP 名单里的用户级进程 */
static int CloseUnneededProcesses(void)
{
    DWORD myPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int killed = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            DWORD pid = pe.th32ProcessID;
            if (pid <= 4 || pid == myPid) continue;

            wchar_t name[128];
            wcsncpy(name, pe.szExeFile, 127);
            name[127] = 0;
            wchar_t low[128];
            wcscpy(low, name);
            ToLowerW(low);

            /* 去掉 .exe 后缀再比较 */
            wchar_t bare[128];
            wcscpy(bare, low);
            StripExe(bare);

            if (IsGameKeep(bare)) continue;

            /* 只处理用户会话进程，跳过服务会话 */
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!hp) continue;
            DWORD session = 0;
            ProcessIdToSessionId(pid, &session);
            CloseHandle(hp);
            if (session == 0) continue;   /* 系统服务不碰 */

            /* 检查是否有可见窗口 或 占用>30MB —— 只关"应用"，不关后台小进程 */
            int shouldKill = 0;
            HANDLE hq = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (hq) {
                PROCESS_MEMORY_COUNTERS pmc;
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hq, &pmc, sizeof(pmc))) {
                    if (pmc.WorkingSetSize > (30ULL * 1024 * 1024)) shouldKill = 1;
                }
                CloseHandle(hq);
            }
            if (!shouldKill) continue;

            HANDLE hk = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hk) {
                if (TerminateProcess(hk, 0)) {
                    killed++;
                    LogMsg(L"closed %s (pid=%lu)", bare, pid);
                }
                CloseHandle(hk);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return killed;
}

/* 恢复指定程序 */
static void RestoreAfterGame(void)
{
    for (int i = 0; RESTORE_AFTER_GAME[i]; ++i) {
        const wchar_t* path = RESTORE_AFTER_GAME[i];
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;

        /* 检查是否已在运行 */
        wchar_t exeName[128];
        const wchar_t* base = wcsrchr(path, L'\\');
        if (base) base++; else base = path;
        wcscpy(exeName, base);
        StripExe(exeName);

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        int running = 0;
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    wchar_t low[128];
                    wcsncpy(low, pe.szExeFile, 127);
                    low[127] = 0;
                    ToLowerW(low);
                    StripExe(low);
                    if (_wcsicmp(low, exeName) == 0) { running = 1; break; }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (running) continue;

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));
        wchar_t cmd[MAX_PATH + 8];
        _snwprintf(cmd, MAX_PATH + 7, L"\"%s\"", path);
        cmd[MAX_PATH + 7] = 0;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            LogMsg(L"restored %s", exeName);
        }
    }
}

static void SlimSelf(void) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA, FALSE, GetCurrentProcessId());
    if (h) { EmptyWorkingSet(h); CloseHandle(h); }
}

/* 前置声明（定义在后面）*/
static int  GetMemTop(MemTopItem* out, int maxN);
static void LogMemTop(void);

// ==================== 系统体检：内存压缩 / 卡顿取证 / 网络 ====================
/*
全部无感：不弹窗、不打扰，只往日志里写。
*/

/* ---- 1. 内存压缩器监控 ---- */
typedef struct {
    unsigned long long compMB;   /* 压缩器占用 */
    unsigned long long freeMB;   /* 可用物理内存 */
    unsigned long long totalMB;
    unsigned long long standbyMB;
} MemHealth;

static int GetMemHealth(MemHealth* out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0;
    out->freeMB  = ms.ullAvailPhys / (1024ULL * 1024ULL);
    out->totalMB = ms.ullTotalPhys / (1024ULL * 1024ULL);

    /* 压缩器是个叫 "Memory Compression" 的进程（pid 通常很小）*/
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 1;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t name[64];
            wcsncpy(name, pe.szExeFile, 63);
            name[63] = 0;
            ToLowerW(name);
            if (wcsstr(name, L"memory compression") || wcsstr(name, L"memcompression")) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (h) {
                    PROCESS_MEMORY_COUNTERS_EX pmc;
                    pmc.cb = sizeof(pmc);
                    if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
                        out->compMB = pmc.WorkingSetSize / (1024ULL * 1024ULL);
                    CloseHandle(h);
                }
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    /* standby 缓存大小（用 SystemMemoryListInformation 拿不准，略过）*/
    return 1;
}

/* 内存体检日志：每 10 分钟一行，只在指标异常时额外提醒 */
static void LogMemHealth(void)
{
    MemHealth mh;
    if (!GetMemHealth(&mh)) return;

    LogMsg(L"health: free=%lluMB/%lluMB, compression=%lluMB",
           mh.freeMB, mh.totalMB, mh.compMB);

    /* 压缩器占超过 1GB 说明系统在疯狂换页，内存真的紧 */
    if (mh.compMB > 1024) {
        LogMsg(L"health WARN: compression %lluMB > 1GB, memory pressure high", mh.compMB);
        LogMemTop();
    } else if (mh.freeMB < 1024) {
        LogMsg(L"health WARN: free %lluMB < 1GB", mh.freeMB);
        LogMemTop();
    }
}

/* 把内存 TOP 写进日志（卡顿取证/内存告警时用）*/
static void LogMemTop(void)
{
    MemTopItem top[MEMTOP_MAX];
    int n = GetMemTop(top, 10);
    for (int i = 0; i < n; ++i) {
        if (top[i].wsMB < 50) break;
        LogMsg(L"  memtop#%d %s %lluMB", i + 1, top[i].name, top[i].wsMB);
    }
}

/* ---- 2. 卡顿取证 ---- */
/* 卡顿归因：记下卡顿瞬间谁在吃 CPU。用 GetProcessTimes 的增量算，
   采样口径和 StallCheck 的窗口对齐，能直接看出是哪个进程把主循环顶住了。 */
typedef struct { DWORD pid; unsigned long long cpu100; } CpuSample;

static void LogStallCulprits(DWORD windowMs)
{
    static CpuSample  s_prev[512];
    static int        s_prevN = 0;
    static DWORD      s_prevTick = 0;

    CpuSample cur[512];
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD now = GetTickCount();
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (n >= 512) break;
            if (pe.th32ProcessID <= 4) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            FILETIME c, e, k, u;
            if (GetProcessTimes(h, &c, &e, &k, &u)) {
                ULARGE_INTEGER kk, uu;
                kk.LowPart = k.dwLowDateTime;  kk.HighPart = k.dwHighDateTime;
                uu.LowPart = u.dwLowDateTime;  uu.HighPart = u.dwHighDateTime;
                cur[n].pid = pe.th32ProcessID;
                cur[n].cpu100 = (kk.QuadPart + uu.QuadPart) / 10000ULL;  /* 毫秒 */
                n++;
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    /* 第一次只建基线 */
    if (s_prevTick == 0) {
        memcpy(s_prev, cur, sizeof(CpuSample) * (size_t)n);
        s_prevN = n; s_prevTick = now;
        return;
    }

    double elapsed = (double)(now - s_prevTick);
    if (elapsed < 100.0) elapsed = 100.0;
    DWORD nCpu = 0;
    { /* 逻辑核数，用来把 ms 换算成占用率 */
        SYSTEM_INFO si; GetSystemInfo(&si); nCpu = si.dwNumberOfProcessors; if (!nCpu) nCpu = 1;
    }

    /* 找上一轮的基线 */
    typedef struct { wchar_t name[64]; int pct; } Culprit;
    Culprit cul[8];
    int culN = 0;

    for (int i = 0; i < n; ++i) {
        unsigned long long prev = 0;
        for (int j = 0; j < s_prevN; ++j) {
            if (s_prev[j].pid == cur[i].pid) { prev = s_prev[j].cpu100; break; }
        }
        if (prev == 0 || cur[i].cpu100 < prev) continue;
        unsigned long long delta = cur[i].cpu100 - prev;
        int pct = (int)((double)delta / elapsed / (double)nCpu * 100.0);
        /* 单核跑满在 8 核上只有 12%，按全机百分比筛会漏。
           改用“占单核的百分比”，单核跑满就计 100%。 */
        int corePct = (int)((double)delta / elapsed * 100.0);
        if (corePct < 25) continue;   /* 至少吃掉四分之一核才算可疑 */

        /* 取进程名 */
        wchar_t nm[64] = L"?";
        PROCESSENTRY32W pe2;
        pe2.dwSize = sizeof(pe2);
        HANDLE hs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hs != INVALID_HANDLE_VALUE) {
            if (Process32FirstW(hs, &pe2)) {
                do {
                    if (pe2.th32ProcessID == cur[i].pid) {
                        wcsncpy(nm, pe2.szExeFile, 63); nm[63] = 0;
                        ToLowerW(nm); StripExe(nm);
                        break;
                    }
                } while (Process32NextW(hs, &pe2));
            }
            CloseHandle(hs);
        }

        /* 插入排序进前 8 */
        int pos = (culN < 8) ? culN++ : 7;
        while (pos > 0 && cul[pos - 1].pct < pct) {
            if (pos < 8) cul[pos] = cul[pos - 1];
            pos--;
        }
        wcsncpy(cul[pos].name, nm, 63); cul[pos].name[63] = 0;
        cul[pos].pct = corePct;
    }

    for (int i = 0; i < culN; ++i)
        LogMsg(L"  stall#%d %s %d%%core", i + 1, cul[i].name, cul[i].pct);
    if (culN == 0)
        LogMsg(L"  stall: no process over 25%% of one core (system-wide/disk/driver stall)");

    memcpy(s_prev, cur, sizeof(CpuSample) * (size_t)n);
    s_prevN = n; s_prevTick = now;
}


/* 卡顿定义：主循环发现自己睡过头了（实际耗时远超预期），说明系统卡了。
   记下当时的 CPU/内存/TOP 进程，方便事后定位。 */
typedef struct {
    DWORD lastTick;
    int   stalls;          /* 累计卡顿次数 */
    int   reported;        /* 已记录次数（防止日志刷屏）*/
} StallWatch;

static StallWatch g_stall = { 0, 0, 0 };

static void StallCheck(DWORD expectedMs)
{
    DWORD now = GetTickCount();
    if (g_stall.lastTick == 0) { g_stall.lastTick = now; return; }

    DWORD actual = now - g_stall.lastTick;
    g_stall.lastTick = now;

    /* 比预期晚 1.5 秒以上就算卡了 */
    if (actual < expectedMs + 1500) return;

    g_stall.stalls++;
    /* 最多记 20 次，避免日志被淹没 */
    if (g_stall.reported >= 20) return;
    g_stall.reported++;

    MemHealth mh;
    GetMemHealth(&mh);
    int cpu = GetCpuUsage();
    int gpu = GetGpuUsage();

    LogMsg(L"STALL #%d: expected %lums, actual %lums (+%lums), cpu=%d%% gpu=%d%% free=%lluMB comp=%lluMB",
           g_stall.stalls, expectedMs, actual, actual - expectedMs,
           cpu, gpu, mh.freeMB, mh.compMB);
    LogStallCulprits(actual);
    LogMemTop();
}

// ==================== 后台进程 CPU / IO 动态调度 ====================
/*
思路：前台程序要响应快，后台程序不要抢。
- 前台进程（当前窗口所属进程 + 其父进程）：不要动，保持原样
- 后台普通程序：EcoQoS 开启（CPU 节能）+ IO 优先级降级
- 深度后台（NEVER_TOUCH 之外的长期不用）：再加 BELOW_NORMAL

绝不 Suspended，绝不停进程，随时可以原地恢复。
*/

static int g_ecoCount = 0;   /* 上一轮降级的进程数，供日志 */

/* 降低进程 IO 优先级（后台读写不跟游戏抢磁盘）*/
static void SetIoPriorityLow(HANDLE h)
{
    typedef NTSTATUS (NTAPI *PFN_NtSetInformationProcess)(HANDLE, ULONG, PVOID, ULONG);
    static PFN_NtSetInformationProcess pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) pfn = (PFN_NtSetInformationProcess)GetProcAddress(nt, "NtSetInformationProcess");
    }
    if (!pfn) return;
    /* ProcessIoPriority = 33, 0=VeryLow 1=Low 2=Normal 3=High */
    ULONG prio = 0;   /* VeryLow */
    pfn(h, 33, &prio, sizeof(prio));
}

/* 按进程名精确优化：只动指定名字的进程，其他一律不碰。
   depth: 1=清工作集，2=再降优先级。返回优化了几个进程。 */
static int TrimByProcName(const wchar_t* target, int depth)
{
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return 0; }

    do {
        DWORD pid = pe.th32ProcessID;
        if (pid <= 4) continue;

        wchar_t name[64];
        wcsncpy(name, pe.szExeFile, 63);
        name[63] = 0;
        ToLowerW(name);
        StripExe(name);
        if (wcscmp(name, target) != 0) continue;

        if (InList(name, NEVER_TOUCH)) continue;

        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA |
                                PROCESS_SET_INFORMATION, FALSE, pid);
        if (!h) continue;

        SetMemoryPriority(h, 1);
        SetPagePriority(h, -1);
        EmptyWorkingSet(h);
        SetMemoryPriority(h, 1);

        if (depth >= 2) {
            SetEco(h, TRUE);
            SetIoPriorityLow(h);
        }
        CloseHandle(h);
        count++;
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return count;
}

/* 把所有非关键进程的 EcoQoS 关掉、优先级还原。
   关闭后台降级开关时调一次，不让任何进程留着降级状态。 */
static int RestoreAllBackground(void)
{
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return 0; }

    do {
        DWORD pid = pe.th32ProcessID;
        if (pid <= 4) continue;

        wchar_t name[64];
        wcsncpy(name, pe.szExeFile, 63);
        name[63] = 0;
        ToLowerW(name);
        StripExe(name);

        /* 系统进程不碰 */
        if (InList(name, NEVER_TOUCH)) continue;

        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA, FALSE, pid);
        if (!h) continue;

        SetEco(h, FALSE);
        /* 只把被我们降到 BELOW_NORMAL 的抬回来，不覆盖用户自己设的 */
        DWORD cls = GetPriorityClass(h);
        if (cls == BELOW_NORMAL_PRIORITY_CLASS || cls == IDLE_PRIORITY_CLASS)
            SetPriorityClass(h, NORMAL_PRIORITY_CLASS);

        CloseHandle(h);
        count++;
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return count;
}

/* 把后台进程降级。前台进程和 NEVER_TOUCH 名单一律不动。
   返回降级的进程数。 */
static int ApplyBackgroundEco(void)
{
    DWORD fgPid = GetFgPid();
    DWORD myPid = GetCurrentProcessId();

    /* 开销大的判断放循环外，别对每个进程跑一遍 */
    int fullscreen = IsForegroundFullscreen();

    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return 0; }

    do {
        DWORD pid = pe.th32ProcessID;
        if (pid <= 4 || pid == myPid) continue;

        wchar_t name[64];
        wcsncpy(name, pe.szExeFile, 63);
        name[63] = 0;
        ToLowerW(name);
        StripExe(name);

        /* 系统关键进程永不动 */
        if (InList(name, NEVER_TOUCH)) continue;

        /* 前台进程：主动恢复优先级，保证响应快 */
        if (pid == fgPid) {
            HANDLE hf = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA, FALSE, pid);
            if (hf) {
                SetPriorityClass(hf, NORMAL_PRIORITY_CLASS);
                SetEco(hf, FALSE);
                CloseHandle(hf);
            }
            continue;
        }

        /* 全屏时不动任何人（游戏/全屏视频优先）*/
        if (fullscreen) continue;

        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA |
                               PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;

        SetEco(h, TRUE);        /* CPU 走节能调度 */
        SetIoPriorityLow(h);    /* 磁盘 IO 排到后面 */
        count++;

        CloseHandle(h);
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return count;
}

// ==================== 内存占用排行 ====================

static int CompareMemTop(const void* a, const void* b)
{
    const MemTopItem* x = (const MemTopItem*)a;
    const MemTopItem* y = (const MemTopItem*)b;
    if (x->wsMB < y->wsMB) return 1;
    if (x->wsMB > y->wsMB) return -1;
    return 0;
}

/* 取内存占用前 N 名（按进程名合并同名进程）*/
static int GetMemTop(MemTopItem* out, int maxN)
{
    MemTopItem tmp[512];
    int n = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return 0; }

    do {
        DWORD pid = pe.th32ProcessID;
        if (pid <= 4) continue;

        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;

        PROCESS_MEMORY_COUNTERS_EX pmc;
        pmc.cb = sizeof(pmc);
        unsigned long long ws = 0;
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
            ws = pmc.WorkingSetSize / (1024ULL * 1024ULL);
        CloseHandle(h);
        if (ws == 0) continue;

        wchar_t name[64];
        wcsncpy(name, pe.szExeFile, 63);
        name[63] = 0;
        ToLowerW(name);
        StripExe(name);

        /* 同名进程合并 */
        int merged = 0;
        for (int i = 0; i < n; ++i) {
            if (wcscmp(tmp[i].name, name) == 0) {
                tmp[i].wsMB += ws;
                merged = 1;
                break;
            }
        }
        if (!merged && n < 512) {
            wcsncpy(tmp[n].name, name, 63);
            tmp[n].name[63] = 0;
            tmp[n].wsMB = ws;
            n++;
        }
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);

    qsort(tmp, (size_t)n, sizeof(MemTopItem), CompareMemTop);

    int cnt = (n < maxN) ? n : maxN;
    for (int i = 0; i < cnt; ++i) out[i] = tmp[i];
    return cnt;
}

/* ---- 3. 网络延迟监控 ---- */
/* 无感：后台 ping 一个稳定地址，只在延迟异常时记日志。
   用 IcmpSendEcho（icmp.dll），不依赖外部 exe。 */
typedef struct {
    DWORD lastCheck;
    int   lastMs;
    int   fails;
    int   worstMs;
    int   samples;
} NetWatch;

static NetWatch g_net = { 0, -1, 0, 0, 0 };

/* 网络体检累计数据（无感，只写日志）*/
typedef struct {
    int samples;
    int fails;
    int good;
    int sumMs;
    int worstMs;
    int jitterSum;      /* 相对上一次延迟的波动绝对值累加 */
    int jitterMax;
    int spikeCount;     /* >150ms 的次数 */
} NetStats;

static NetStats g_netStats = { 0, 0, 0, 0, 0, 0, 0, 0 };

/* gethostbyname 依赖 Winsock，不初始化直接调必失败。
   之前延迟监控从来没生效过（静默返回 -1），这里一次性初始化好。 */
static void NetInitOnce(void)
{
    static int inited = 0;
    if (inited) return;
    inited = 1;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

static int PingOnce(const wchar_t* host, int timeoutMs)
{
    static HANDLE (WINAPI *pIcmpCreateFile)(void) = NULL;
    static DWORD  (WINAPI *pIcmpSendEcho)(HANDLE,DWORD,LPVOID,WORD,IP_OPTION_INFORMATION*,LPVOID,DWORD,DWORD) = NULL;
    static BOOL   (WINAPI *pIcmpCloseHandle)(HANDLE) = NULL;
    static int inited = 0;

    if (!inited) {
        inited = 1;
        HMODULE ic = LoadLibraryW(L"icmp.dll");
        if (ic) {
            pIcmpCreateFile  = (void*)GetProcAddress(ic, "IcmpCreateFile");
            pIcmpSendEcho    = (void*)GetProcAddress(ic, "IcmpSendEcho");
            pIcmpCloseHandle = (void*)GetProcAddress(ic, "IcmpCloseHandle");
        }
    }
    if (!pIcmpCreateFile || !pIcmpSendEcho) return -1;

    NetInitOnce();

    /* 解析主机名 */
    char hostA[128];
    WideCharToMultiByte(CP_ACP, 0, host, -1, hostA, 127, NULL, NULL);
    hostA[127] = 0;

    struct hostent* he = gethostbyname(hostA);
    if (!he || !he->h_addr_list[0]) return -1;
    DWORD addr = *(DWORD*)he->h_addr_list[0];

    HANDLE h = pIcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) return -1;

    char send[32] = "MemTune";
    char reply[sizeof(ICMP_ECHO_REPLY) + 64];
    /* 参数顺序：(handle, dest, request, reqSize, options, reply, replySize, timeout) */
    DWORD r = pIcmpSendEcho(h, addr, send,
                            (WORD)strlen(send),
                            NULL,
                            reply,
                            (DWORD)sizeof(reply),
                            (DWORD)timeoutMs);
    pIcmpCloseHandle(h);

    if (r == 0) return -1;
    ICMP_ECHO_REPLY* rep = (ICMP_ECHO_REPLY*)reply;
    if (rep->Status != 0) return -1;
    return (int)rep->RoundTripTime;
}

/* 每 3 分钟 ping 一次，只在异常时记日志 */
static void NetCheck(void)
{
    DWORD now = GetTickCount();
    if (g_net.lastCheck != 0 && (now - g_net.lastCheck) < 180000) return;
    g_net.lastCheck = now;

    int ms = PingOnce(L"223.5.5.5", 1500);   /* 阿里 DNS，国内稳定 */

    if (ms < 0) {
        g_net.fails++;
        g_netStats.fails++;
        if (g_net.fails >= 3) {
            LogMsg(L"net WARN: %d consecutive ping failures, network may be down", g_net.fails);
            g_net.fails = 0;
        }
        return;
    }

    int prevMs = g_net.lastMs;
    g_net.fails = 0;
    g_net.lastMs = ms;
    g_net.samples++;
    if (ms > g_net.worstMs) g_net.worstMs = ms;

    /* 累计统计 */
    g_netStats.samples++;
    g_netStats.sumMs += ms;
    if (ms > g_netStats.worstMs) g_netStats.worstMs = ms;
    if (prevMs >= 0) {
        int d = ms - prevMs; if (d < 0) d = -d;
        g_netStats.jitterSum += d;
        if (d > g_netStats.jitterMax) g_netStats.jitterMax = d;
        /* 抖动超过 40ms 说明链路不稳，打游戏会跳 ping */
        if (d > 40)
            LogMsg(L"net JITTER: %dms -> %dms (delta %dms)", prevMs, ms, d);
    }

    /* 延迟超过 150ms 才算异常（国内一般 <50ms），每 5 分钟最多报一次 */
    if (ms > 150) {
        g_netStats.spikeCount++;
        static DWORD s_lastSpikeLog = 0;
        DWORD t = GetTickCount();
        if (s_lastSpikeLog == 0 || (t - s_lastSpikeLog) > 300000) {
            s_lastSpikeLog = t;
            LogMsg(L"net WARN: latency %dms (>150ms), worst=%dms avg=%dms",
                   ms, g_netStats.worstMs,
                   g_netStats.samples ? g_netStats.sumMs / g_netStats.samples : ms);
        }
    }

    /* 每 6 小时汇总一行，长期看网络质量 */
    {
        static DWORD s_lastSummary = 0;
        DWORD t = GetTickCount();
        if (s_lastSummary == 0) s_lastSummary = t;
        if ((t - s_lastSummary) > 21600000 && g_netStats.samples > 0) {
            s_lastSummary = t;
            LogMsg(L"net summary: samples=%d avg=%dms worst=%dms jitter(avg=%dms max=%dms) spikes=%d fails=%d",
                   g_netStats.samples,
                   g_netStats.sumMs / g_netStats.samples,
                   g_netStats.worstMs,
                   g_netStats.jitterSum / (g_netStats.samples > 1 ? g_netStats.samples - 1 : 1),
                   g_netStats.jitterMax,
                   g_netStats.spikeCount,
                   g_netStats.fails);
            memset(&g_netStats, 0, sizeof(g_netStats));
        }
    }
}

/* ---- 4. 定时自动优化 ---- */
/* 空闲一段时间后自动 trim 一次。无感：不弹窗，只记日志。 */
#define AUTO_TRIM_INTERVAL_MS  (15 * 60 * 1000)   /* 15 分钟 */
#define AUTO_TRIM_MIN_IDLE_MS  300000             /* 至少 5 分钟没操作才动手 */

static void AutoTrimTick(void)
{
    static DWORD s_last = 0;
    DWORD now = GetTickCount();
    if (s_last != 0 && (now - s_last) < AUTO_TRIM_INTERVAL_MS) return;
    s_last = now;

    /* 用户正在操作（有键鼠输入）就不打扰 */
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    if (GetLastInputInfo(&lii)) {
        DWORD idleMs = now - lii.dwTime;
        if (idleMs < AUTO_TRIM_MIN_IDLE_MS) return;
    }

    MemHealth mh;
    GetMemHealth(&mh);

    /* 内存宽裕（可用 >4GB 且压缩器 <512MB）就不折腾 */
    if (mh.freeMB > 4096 && mh.compMB < 512) return;

    double before = GetFreeGB();
    TrimResult r = TrimProcsMulti(IDLE_TRIM_ABOVE_MB, 0, 2);
    double after = GetFreeGB();

    LogMsg(L"auto trim (idle): trim=%d freed=%lluMB free=%.2f->%.2fGB comp=%lluMB",
           r.trimmed, r.freedMB, before, after, mh.compMB);
}

// ==================== 内存泄漏检测与自动处理 ====================
/*
思路：真正的内存泄漏 = 私有内存「只涨不跌」。

区分两种情况（很重要）：
  1. 缓存型增长：涨上去但会回落（浏览器、图片缓存）—— 正常，不管
  2. 泄漏型增长：一直在涨，从不回落 —— 才是泄漏

采样方式：
- 每 60 秒采样一次所有进程的 PrivateUsage
- 每个进程保留最近 LEAK_SAMPLES 个点（环形）
- 判定：窗口内最低点不降反升，且总涨幅超过阈值，且持续 N 个窗口

分级处理（绝不直接杀进程）：
  等级1  轻度：清一次工作集 + 降内存优先级
  等级2  中度：再降 CPU 优先级 + 记日志
  等级3  重度：写详细报告，等用户处理（或按配置强制结束）
*/

#define LEAK_SAMPLES        20          /* 保留 20 个采样点（约 20 分钟）*/
#define LEAK_SAMPLE_MS      60000        /* 60 秒采一次 */
#define LEAK_MIN_GROWTH_MB  200          /* 至少涨了 200MB 才算 */
#define LEAK_MIN_SIZE_MB    150          /* 小于这个体积不查（小进程波动大）*/
#define LEAK_STREAK_WARN    3            /* 连续 3 次判定才确认泄漏 */
#define LEAK_MAX_TRACK      256          /* 最多同时跟踪 256 个进程 */

typedef struct {
    DWORD pid;
    wchar_t name[64];
    unsigned long long samples[LEAK_SAMPLES];   /* 私有内存 KB */
    int   head;         /* 环形写位置 */
    int   count;        /* 有效采样数 */
    int   streak;       /* 连续判定泄漏的窗口数 */
    int   stage;        /* 已处理到哪一级：0 未处理 1 轻 2 中 3 重 */
    DWORD lastTick;
} LeakTrack;

static LeakTrack g_leak[LEAK_MAX_TRACK];
static int       g_leakN = 0;
static int       g_leakAuto = 1;   /* 自动处理开关 1=开 0=只告警 */

/* 取进程私有内存（KB）—— 私有内存不受工作集清理影响，能真实反映申请量 */
static unsigned long long GetPrivateKB(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    unsigned long long r = 0;
    if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        r = (unsigned long long)pmc.PrivateUsage / 1024ULL;
    CloseHandle(h);
    return r;
}

static LeakTrack* LeakFind(DWORD pid)
{
    for (int i = 0; i < g_leakN; ++i)
        if (g_leak[i].pid == pid) return &g_leak[i];
    return NULL;
}

static LeakTrack* LeakAdd(DWORD pid, const wchar_t* name)
{
    LeakTrack* t = LeakFind(pid);
    if (t) return t;
    if (g_leakN >= LEAK_MAX_TRACK) return NULL;
    t = &g_leak[g_leakN++];
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    wcsncpy(t->name, name, 63);
    t->name[63] = 0;
    return t;
}

/* 清掉已经退出的进程，保持数组紧凑 */
static void LeakPrune(void)
{
    int w = 0;
    for (int i = 0; i < g_leakN; ++i) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_leak[i].pid);
        if (!h) continue;   /* 进程没了，丢弃 */
        CloseHandle(h);
        if (w != i) g_leak[w] = g_leak[i];
        w++;
    }
    g_leakN = w;
}

/* 计算泄漏判定：返回 1=判定泄漏，growthKB 带回涨幅 */
static int LeakJudged(LeakTrack* t, unsigned long long* growthKB)
{
    if (!t || t->count < 5) return 0;

    /* 取窗口内最小值（最低水位）和最后一个值 */
    unsigned long long minV = (unsigned long long)-1;
    unsigned long long last = 0;
    int idx = t->count < LEAK_SAMPLES ? 0 : t->head;
    for (int k = 0; k < t->count; ++k) {
        int p = (idx + k) % LEAK_SAMPLES;
        if (t->samples[p] < minV) minV = t->samples[p];
    }
    last = t->samples[(t->head + LEAK_SAMPLES - 1) % LEAK_SAMPLES];

    if (minV == (unsigned long long)-1) return 0;
    if (minV < LEAK_MIN_SIZE_MB * 1024ULL) return 0;   /* 太小不查 */
    if (last <= minV) return 0;                        /* 在落就不算泄漏 */

    unsigned long long grow = last - minV;
    if (growthKB) *growthKB = grow;
    if (grow < LEAK_MIN_GROWTH_MB * 1024ULL) return 0;
    return 1;
}

/* 对泄漏进程做处理。stage 0->1->2->3 递进 */
static int LeakHandle(LeakTrack* t, unsigned long long growthKB)
{
    if (!t) return 0;

    int newStage = t->stage + 1;
    if (newStage > 3) newStage = 3;

    unsigned long long mb = growthKB / 1024ULL;
    unsigned long long curMB = t->samples[(t->head + LEAK_SAMPLES - 1) % LEAK_SAMPLES] / 1024ULL;

    if (newStage == 1) {
        /* 等级1：清工作集 + 降内存优先级。不伤害功能，只是把已用部分换出去。 */
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA |
                               PROCESS_SET_INFORMATION, FALSE, t->pid);
        if (h) {
            SetMemoryPriority(h, 1);
            SetPagePriority(h, -1);
            EmptyWorkingSet(h);
            CloseHandle(h);
        }
        LogMsg(L"leak L1 [%s pid=%lu]: +%lluMB to %lluMB, working set trimmed",
               t->name, t->pid, mb, curMB);
    }
    else if (newStage == 2) {
        /* 等级2：再降 CPU 优先级 + IO 优先级。让它别抢资源。 */
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
                               FALSE, t->pid);
        if (h) {
            SetEco(h, TRUE);
            SetIoPriorityLow(h);
            SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
            CloseHandle(h);
        }
        LogMsg(L"leak L2 [%s pid=%lu]: +%lluMB to %lluMB, cpu/io priority lowered",
               t->name, t->pid, mb, curMB);
    }
    else {
        /* 等级3：只写详细报告，不杀。用户自己决定。 */
        LogMsg(L"leak L3 [%s pid=%lu]: +%lluMB to %lluMB -- STILL GROWING, manual action needed",
               t->name, t->pid, mb, curMB);
        LogMsg(L"  hint: this process keeps allocating and never releases; consider restarting it");
    }

    t->stage = newStage;
    return 1;
}

/* 主入口：每轮主循环调用，自己控频率 */
static void LeakTick(void)
{
    static DWORD s_last = 0;
    DWORD now = GetTickCount();
    if (s_last != 0 && (now - s_last) < LEAK_SAMPLE_MS) return;
    s_last = now;

    LeakPrune();

    /* 采样所有进程 */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            DWORD pid = pe.th32ProcessID;
            if (pid <= 4) continue;

            wchar_t nm[64];
            wcsncpy(nm, pe.szExeFile, 63);
            nm[63] = 0;
            ToLowerW(nm);
            StripExe(nm);

            /* 系统关键进程不跟踪 */
            if (InList(nm, NEVER_TOUCH)) continue;

            unsigned long long kb = GetPrivateKB(pid);
            if (kb < LEAK_MIN_SIZE_MB * 1024ULL) continue;

            LeakTrack* t = LeakAdd(pid, nm);
            if (!t) continue;

            t->samples[t->head] = kb;
            t->head = (t->head + 1) % LEAK_SAMPLES;
            if (t->count < LEAK_SAMPLES) t->count++;
            t->lastTick = now;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    /* 逐个审阅 */
    for (int i = 0; i < g_leakN; ++i) {
        LeakTrack* t = &g_leak[i];
        unsigned long long grow = 0;
        if (LeakJudged(t, &grow)) {
            t->streak++;
            if (t->streak >= LEAK_STREAK_WARN) {
                if (g_leakAuto) LeakHandle(t, grow);
                else if (t->streak == LEAK_STREAK_WARN)
                    LogMsg(L"leak detected [%s pid=%lu]: +%lluMB over %d samples (auto disabled)",
                           t->name, t->pid, grow / 1024ULL, t->count);
            }
        } else {
            /* 回落了，清零计数；已经处理过的保留 stage，不重复处理 */
            if (t->streak > 0) t->streak = 0;
        }
    }
}

// ==================== 三角洲局内/局外 检测 + 抖音限流 ====================
/*
判断原则（多个信号叠加，不靠单一指标）：
  局内：CPU 持续高 + GPU 中高 + 前台窗口存在，连续稳定
  局外（大厅/匹配/加载）：CPU 低或波动大

关键：只降抖音的“工作集上限”，不杀进程、不关窗口，
     切回三角洲后抖音自动恢复，下次还能继续刷（最小化暂停）。
*/

#define DF_PROC            L"deltaforceclient-win64-shipping"
#define DF_WINDOW_CLASS    L"UnrealWindow"
#define DY_PROC            L"douyin"

#define DF_CPU_INGAME      18      /* 三角洲进程 CPU 占用 ≥18% 视为局内 */
#define DF_GPU_INGAME      45      /* 且 GPU ≥45% */
#define DF_INGAME_CONFIRM  3       /* 连续 3 次采样确认，防瞬时尖峰 */
#define DF_IDLE_CONFIRM    5       /* 连续 5 次采样确认局外，防抖动 */

#define DY_INGAME_LIMIT_MB   300   /* 局内：抖音工作集压到 300MB */
#define DY_LIMIT_QUIET_MS    8000  /* 限制后至少安静 8 秒再复查，避免死磕 */

static int g_dfInGame      = 0;   /* 当前判断：1=局内 */
static int g_dfStreak      = 0;   /* 连续计数 */
static int g_dfStreakIdle  = 0;
static DWORD g_dyLimitTick = 0;

/* 取某进程名（不含 .exe，小写）的 CPU 占用百分比 + 工作集。
   同一时刻可能有多个同名进程，全部累加。返回 1 表示找到。 */
/* 取某进程名（不含 .exe，小写）的 CPU 占用百分比 + 工作集。
   同一时刻多个同名进程全部累加。返回 1 表示找到。
   第一次调用只建基线（返回 cpu=0），之后才准。 */
typedef struct { DWORD pid; ULONGLONG k, u; } ProcCpuSample;

static ProcCpuSample g_pcs[128];
static int           g_pcsN    = 0;
static DWORD         g_pcsTick = 0;

static int GetProcStats(const wchar_t* target, int* cpuPct,
                        unsigned long long* wsBytes, DWORD* mainPid)
{
    DWORD nowTick  = GetTickCount();
    DWORD elapsed  = (g_pcsTick == 0) ? 0 : (nowTick - g_pcsTick);

    int    found   = 0;
    double cpuSum  = 0.0;
    unsigned long long wsSum = 0;
    DWORD  bestPid = 0;
    unsigned long long bestWs = 0;

    ProcCpuSample next[128];
    int nextN = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return 0; }

    do {
        wchar_t name[64];
        wcsncpy(name, pe.szExeFile, 63);
        name[63] = 0;
        ToLowerW(name);
        StripExe(name);
        if (wcscmp(name, target) != 0) continue;

        DWORD pid = pe.th32ProcessID;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;

        FILETIME a1, a2, kt, ut;
        ULONGLONG k = 0, u = 0;
        int gotTimes = 0;
        if (GetProcessTimes(h, &a1, &a2, &kt, &ut)) {
            k = ((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
            u = ((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
            gotTimes = 1;
        }

        PROCESS_MEMORY_COUNTERS_EX pmc;
        pmc.cb = sizeof(pmc);
        unsigned long long ws = 0;
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
            ws = pmc.WorkingSetSize;
        CloseHandle(h);

        found = 1;
        wsSum += ws;
        if (ws > bestWs) { bestWs = ws; bestPid = pid; }

        if (gotTimes) {
            if (nextN < 128) {
                next[nextN].pid = pid;
                next[nextN].k   = k;
                next[nextN].u   = u;
                nextN++;
            }
            /* 和上一轮同一 pid 比，算出这段时间吃了多少 CPU */
            if (elapsed > 0) {
                for (int i = 0; i < g_pcsN; ++i) {
                    if (g_pcs[i].pid != pid) continue;
                    ULONGLONG prev = g_pcs[i].k + g_pcs[i].u;
                    ULONGLONG cur  = k + u;
                    if (cur > prev) {
                        double secs = (double)(cur - prev) / 10000000.0;
                        cpuSum += secs / ((double)elapsed / 1000.0) * 100.0;
                    }
                    break;
                }
            }
        }
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);

    memcpy(g_pcs, next, sizeof(ProcCpuSample) * (size_t)nextN);
    g_pcsN   = nextN;
    g_pcsTick = nowTick;

    if (!found) return 0;

    if (cpuPct)  *cpuPct  = (int)(cpuSum + 0.5);
    if (wsBytes) *wsBytes = wsSum;
    if (mainPid) *mainPid = bestPid;
    return 1;
}

/* 三角洲是否在前台（不管全屏还是窗口）*/
static int DeltaForceForeground(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid <= 4) return 0;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;

    wchar_t path[MAX_PATH] = L"";
    DWORD sz = MAX_PATH;
    int ok = 0;
    if (QueryFullProcessImageNameW(h, 0, path, &sz)) {
        ToLowerW(path);
        if (wcsstr(path, L"deltaforceclient")) ok = 1;
    }
    CloseHandle(h);
    return ok;
}

/* 把抖音的工作集压到 limitMB 以内（软限制：清一次工作集并降优先级）*/
static int LimitDouyin(unsigned long long limitMB)
{
    int touched = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return 0; }

    do {
        wchar_t name[64];
        wcsncpy(name, pe.szExeFile, 63);
        name[63] = 0;
        ToLowerW(name);
        StripExe(name);
        if (wcsncmp(name, DY_PROC, 6) != 0) continue;

        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA |
                               PROCESS_SET_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;

        PROCESS_MEMORY_COUNTERS_EX pmc;
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            unsigned long long ws = pmc.WorkingSetSize;
            if (ws > limitMB * 1024ULL * 1024ULL) {
                SetMemoryPriority(h, 1);   /* VeryLow：换页时优先挑它 */
                SetPagePriority(h, -1);
                EmptyWorkingSet(h);
                SetMemoryPriority(h, 1);
                touched = 1;
            }
        }
        CloseHandle(h);
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return touched;
}

/* 更新局内/局外状态，并在局内时限制抖音。返回 1=局内 */
static int UpdateDeltaForceState(int gpu)
{
    static int s_wasRunning = 0;

    int dfCpu = 0;
    unsigned long long dfWs = 0;
    int running = GetProcStats(DF_PROC, &dfCpu, &dfWs, NULL);

    g_lastDfRun = running;
    g_lastDfCpu = dfCpu;

    /* 预优化已交给通用的 GamePreOptTick（主循环调用，覆盖 games.txt 全部游戏），
       这里不再重复做，避免同一轮清两遍。 */
    if (running && !s_wasRunning)
        LogMsg(L"DF process detected");

    if (!running) {
        s_wasRunning = 0;
        g_dfInGame = 0;
        g_dfStreak = 0;
        g_dfStreakIdle = 0;
        return 0;
    }
    s_wasRunning = 1;

    int looksInGame = (dfCpu >= DF_CPU_INGAME) && (gpu >= DF_GPU_INGAME);

    /* 采样日志：每 30 秒记一次，方便实测后校准阈值 */
    {
        static DWORD s_lastLog = 0;
        DWORD t = GetTickCount();
        if (s_lastLog == 0 || (t - s_lastLog) > 30000) {
            LogMsg(L"DF probe: cpu=%d%% gpu=%d%% ws=%lluMB -> %s",
                   dfCpu, gpu, dfWs / (1024ULL * 1024ULL),
                   looksInGame ? L"in" : L"out");
            s_lastLog = t;
        }
    }

    if (looksInGame) {
        g_dfStreak++;
        g_dfStreakIdle = 0;
        if (g_dfStreak >= DF_INGAME_CONFIRM && !g_dfInGame) {
            g_dfInGame = 1;
            LogMsg(L"DF -> IN GAME (cpu=%d%% gpu=%d%%)", dfCpu, gpu);
        }
    } else {
        g_dfStreakIdle++;
        g_dfStreak = 0;
        if (g_dfStreakIdle >= DF_IDLE_CONFIRM && g_dfInGame) {
            g_dfInGame = 0;
            LogMsg(L"DF -> OUT OF GAME (cpu=%d%% gpu=%d%%)", dfCpu, gpu);
        }
    }

    /* 局内才限制抖音，局外完全不管 */
    if (g_dfInGame) {
        DWORD now = GetTickCount();
        if (g_dyLimitTick == 0 || (now - g_dyLimitTick) > DY_LIMIT_QUIET_MS) {
            if (LimitDouyin(DY_INGAME_LIMIT_MB)) {
                LogMsg(L"douyin working set trimmed to <=%dMB", DY_INGAME_LIMIT_MB);
            }
            g_dyLimitTick = now;
        }
    } else {
        g_dyLimitTick = 0;
    }

    return g_dfInGame;
}

// ==================== 磁盘垃圾清理 ====================
/*
  只清「公认可安全删除」的位置。红线：
  1. 绝不碰任何用户文档 / 下载 / 桌面 / 游戏目录 / 程序目录
  2. 只删明确属于缓存/临时/日得的位置
  3. 删除失败的跳过（不报错、不卡住）
  4. 先算体积再删，超过上限的单个目录只统计不删（防止误伤）
  5. 不按名字猜：
     "Cache" 这种名字背后可能是微信聊天媒体目录（D:\Cache\WX），
     删了就是真数据丢失。只清白名单里写死的系统级路径。
*/

#define CLEAN_MAX_DEPTH   6
#define CLEAN_BUDGET_MS   60000   /* 单次清理最长 60 秒 */
#define CLEAN_MAX_DIR_MB  2048.0  /* 单个目录超过 2GB 不删（可能是真数据）*/
#define CLEAN_DRYRUN      0       /* 1 = 只统计不删 */

typedef struct {
    unsigned long long bytes;
    int                files;
    int                errs;
} CleanStat;

static DWORD g_cleanStart = 0;

static int CleanTimedOut(void)
{
    return (GetTickCount() - g_cleanStart) > CLEAN_BUDGET_MS;
}

/* 先算目录体积（只统计，不删）*/
static unsigned long long MeasureDir(const wchar_t* dir, int depth)
{
    if (depth > CLEAN_MAX_DEPTH || CleanTimedOut()) return 0;
    wchar_t pat[MAX_PATH + 8];
    _snwprintf(pat, MAX_PATH + 7, L"%s\\*", dir);
    pat[MAX_PATH + 7] = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    unsigned long long total = 0;
    do {
        if (CleanTimedOut()) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[MAX_PATH * 2];
        _snwprintf(full, MAX_PATH * 2 - 1, L"%s\\%s", dir, fd.cFileName);
        full[MAX_PATH * 2 - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            total += MeasureDir(full, depth + 1);
        } else {
            total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

/* 递归删除目录内容，但保留目录本身 */
static void CleanDirRecursive(const wchar_t* dir, CleanStat* st, int depth)
{
    if (!st || depth > CLEAN_MAX_DEPTH) return;
    if (CleanTimedOut()) return;

    wchar_t pat[MAX_PATH + 8];
    _snwprintf(pat, MAX_PATH + 7, L"%s\\*", dir);
    pat[MAX_PATH + 7] = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (CleanTimedOut()) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        wchar_t full[MAX_PATH * 2];
        _snwprintf(full, MAX_PATH * 2 - 1, L"%s\\%s", dir, fd.cFileName);
        full[MAX_PATH * 2 - 1] = 0;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;   /* 不跟 junctions / 符号链接，防止跳出去误删 */
            CleanDirRecursive(full, st, depth + 1);
            RemoveDirectoryW(full);   /* 内容清空后删掉空目录 */
        } else {
            unsigned long long sz =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(full)) {
                st->bytes += sz;
                st->files++;
            } else {
                st->errs++;
            }
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

/* 展开环境变量后的路径是否存在 */
static int ExpandExists(const wchar_t* raw, wchar_t* out, int outChars)
{
    DWORD n = ExpandEnvironmentStringsW(raw, out, (DWORD)outChars);
    if (n == 0 || n > (DWORD)outChars) return 0;
    DWORD a = GetFileAttributesW(out);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
}

/* ---- 安全清理目标（全部是系统/软件缓存，不含任何用户数据）---- */
static const wchar_t* CLEAN_TARGETS_COMMON[] = {
    /* Windows 临时文件 */
    L"%LOCALAPPDATA%\\Temp",
    L"%WINDIR%\\Temp",
    /* 缩略图 / 图标缓存 */
    L"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer",
    /* 崩溃转储 */
    L"%LOCALAPPDATA%\\CrashDumps",
    L"%WINDIR%\\Minidump",
    /* Windows 更新下载缓存 */
    L"%WINDIR%\\SoftwareDistribution\\Download",
    /* 传递优化缓存 */
    L"%WINDIR%\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization\\Cache",
    /* 安装器缓存 */
    L"%WINDIR%\\Installer\\$PatchCache$",
    /* 错误报告 */
    L"%PROGRAMDATA%\\Microsoft\\Windows\\WER",
    L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER",
    /* 字体/着色器缓存 */
    L"%LOCALAPPDATA%\\D3DSCache",
    L"%LOCALAPPDATA%\\NVIDIA\\DXCache",
    L"%LOCALAPPDATA%\\NVIDIA\\GLCache",
    L"%LOCALAPPDATA%\\AMD\\DxCache",
    /* 浏览器缓存（不碰书签/密码/历史，只删 Cache 子目录）*/
    L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Cache",
    L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Code Cache",
    L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Cache",
    L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Code Cache",
    /* 其他软件缓存 */
    L"%LOCALAPPDATA%\\Microsoft\\Windows\\INetCache",
    L"%LOCALAPPDATA%\\Microsoft\\Windows\\WebCache",
    L"%LOCALAPPDATA%\\Packages\\Microsoft.WindowsStore_8wekyb3d8bbwe\\LocalCache",
    NULL
};

/* 统计单个盘回收站占用（读 $Recycle.Bin 体积）*/
static unsigned long long MeasureRecycleBin(wchar_t drive)
{
    wchar_t root[16];
    _snwprintf(root, 15, L"%c:\\$Recycle.Bin", drive);
    root[15] = 0;
    DWORD a = GetFileAttributesW(root);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;

    /* 不要受 60 秒预算影响，否则量不准 */
    DWORD save = g_cleanStart;
    g_cleanStart = GetTickCount();
    unsigned long long sz = MeasureDir(root, 0);
    g_cleanStart = save;
    return sz;
}

/* 清理回收站。
   drive = 0 清所有盘，否则只清指定盘。
   先量体积（$Recycle.Bin）再调 API，因为 SHEmptyRecycleBin 不返回释放了多少。 */
static void CleanRecycleBin(wchar_t drive, CleanStat* st)
{
    /* 先量体积 */
    unsigned long long freed = 0;
    if (drive) {
        freed = MeasureRecycleBin(drive);
    } else {
        for (wchar_t d = L'C'; d <= L'Z'; ++d) {
            wchar_t root[8];
            _snwprintf(root, 7, L"%c:\\", d);
            root[7] = 0;
            if (GetDriveTypeW(root) == DRIVE_FIXED)
                freed += MeasureRecycleBin(d);
        }
    }

    typedef HRESULT (WINAPI *PFN_SHB)(HWND, LPCWSTR, DWORD);
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    if (!sh) return;
    PFN_SHB fn = (PFN_SHB)GetProcAddress(sh, "SHEmptyRecycleBinW");
    if (fn) {
        /* SHERB_NOCONFIRMATION(0x1) | SHERB_NOPROGRESSUI(0x2) | SHERB_NOSOUND(0x4) */
        wchar_t root[8] = L"";
        LPCWSTR pszRoot = NULL;
        if (drive) {
            _snwprintf(root, 7, L"%c:\\", drive);
            root[7] = 0;
            pszRoot = root;
        }
        HRESULT hr = fn(NULL, pszRoot, 0x1 | 0x2 | 0x4);
        if (SUCCEEDED(hr)) {
            if (st && freed > 0) {
                st->bytes += freed;
                LogMsg(L"clean recycle bin (%s): %.1f MB",
                       drive ? root : L"all drives",
                       (double)freed / (1024.0 * 1024.0));
            } else {
                LogMsg(L"clean recycle bin (%s): already empty",
                       drive ? root : L"all drives");
            }
        }
    }
    FreeLibrary(sh);
}

/* 对指定盘符执行清理。drive = 'C' 或 'D'，0 表示全部 */
static void DoCleanDisk(wchar_t drive)
{
    g_cleanStart = GetTickCount();
    CleanStat st;
    memset(&st, 0, sizeof(st));

    double beforeC = 0.0;
    {
        wchar_t root[8];
        _snwprintf(root, 7, L"%c:\\", drive ? drive : L'C');
        root[7] = 0;
        ULARGE_INTEGER fr, tot, totfr;
        if (GetDiskFreeSpaceExW(root, &fr, &tot, &totfr))
            beforeC = (double)fr.QuadPart / (1024.0 * 1024.0 * 1024.0);
    }

    LogMsg(L"clean start (drive=%c, budget=%ds)",
           drive ? drive : L'*', CLEAN_BUDGET_MS / 1000);

    for (int i = 0; CLEAN_TARGETS_COMMON[i]; ++i) {
        if (CleanTimedOut()) {
            LogMsg(L"clean: time budget hit, stopping");
            break;
        }
        wchar_t full[MAX_PATH * 2];
        if (!ExpandExists(CLEAN_TARGETS_COMMON[i], full, MAX_PATH * 2)) continue;

        /* 只在目标盘上执行（全部是系统盘位置，D 盘只清 temp）*/
        wchar_t up = (full[0] >= L'a' && full[0] <= L'z') ? (wchar_t)(full[0] - 32) : full[0];
        if (drive && up != drive) continue;

        unsigned long long b0 = st.bytes;
        int f0 = st.files;

        /* 安全阀：先量体积，超过上限就不删 */
        unsigned long long sz = MeasureDir(full, 0);
        double mb = (double)sz / (1024.0 * 1024.0);
        if (mb > CLEAN_MAX_DIR_MB) {
            LogMsg(L"clean SKIP %s: %.0f MB > %.0f MB limit", full, mb, CLEAN_MAX_DIR_MB);
            continue;
        }

        if (CLEAN_DRYRUN) {
            LogMsg(L"clean DRYRUN %s: %.1f MB", full, mb);
            continue;
        }

        CleanDirRecursive(full, &st, 0);
        unsigned long long got = st.bytes - b0;
        if (got > 0 || st.files > f0)
            LogMsg(L"clean %s: %d files, %.1f MB", full, st.files - f0,
                   (double)got / (1024.0 * 1024.0));
    }

    /* D 盘：额外清固定临时目录（如果存在）。
       只认名字就是 Temp/tmp 的目录。
       教训：D:\Cache 看着像缓存，实际是微信聊天媒体目录，绝不能删。 */
    if (!drive || drive == L'D') {
        static const wchar_t* dTargets[] = {
            L"D:\\Temp", L"D:\\temp", L"D:\\tmp", L"D:\\Tmp",
            NULL
        };
        for (int i = 0; dTargets[i]; ++i) {
            if (CleanTimedOut()) break;
            DWORD a = GetFileAttributesW(dTargets[i]);
            if (a == INVALID_FILE_ATTRIBUTES) continue;
            if (!(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(dTargets[i], L"D:\\$RECYCLE.BIN") == 0) continue; /* 走回收站 API */
            unsigned long long b0 = st.bytes;
            int f0 = st.files;
            CleanDirRecursive(dTargets[i], &st, 0);
            unsigned long long got = st.bytes - b0;
            if (got > 0)
                LogMsg(L"clean %s: %d files, %.1f MB", dTargets[i], st.files - f0,
                       (double)got / (1024.0 * 1024.0));
        }
    }

    /* 回收站：只清当前盘的 */
    CleanRecycleBin(drive, &st);

    double afterC = 0.0;
    {
        wchar_t root[8];
        _snwprintf(root, 7, L"%c:\\", drive ? drive : L'C');
        root[7] = 0;
        ULARGE_INTEGER fr, tot, totfr;
        if (GetDiskFreeSpaceExW(root, &fr, &tot, &totfr))
            afterC = (double)fr.QuadPart / (1024.0 * 1024.0 * 1024.0);
    }

    LogMsg(L"clean done: %d files, %.1f MB, %d skipped, free %.1f -> %.1f GB",
           st.files, (double)st.bytes / (1024.0 * 1024.0), st.errs, beforeC, afterC);
}

// ==================== 托盘窗口 ====================
/* 清理线程：整个扫描+删除可能几十秒，放主线程会卡死托盘和主循环 */
static volatile int g_cleaning = 0;
static volatile wchar_t g_cleanDrive = 0;

static DWORD WINAPI CleanThreadProc(LPVOID param)
{
    wchar_t drive = (wchar_t)(ULONG_PTR)param;
    g_cleaning = 1;

    if (drive == 0) {
        DoCleanDisk(L'C');
        DoCleanDisk(L'D');
    } else {
        DoCleanDisk(drive);
    }

    g_cleaning = 0;
    return 0;
}

/* 只清空回收站（所有固定盘），不动任何缓存 */
static DWORD WINAPI CleanBinThreadProc(LPVOID param)
{
    (void)param;
    g_cleaning = 1;

    CleanStat st;
    memset(&st, 0, sizeof(st));
    g_cleanStart = GetTickCount();

    double before = GetFreeGB();
    LogMsg(L"cleanbin start");

    /* drive=0 表示所有盘 */
    CleanRecycleBin(0, &st);

    double after = GetFreeGB();
    LogMsg(L"cleanbin done: %.1f MB, free %.2f -> %.2f GB",
           (double)st.bytes / (1024.0 * 1024.0), before, after);

    g_cleaning = 0;
    return 0;
}

static void StartCleanBin(void)
{
    if (g_cleaning) {
        LogMsg(L"cleanbin: already running, ignored");
        return;
    }
    g_cleaning = 1;
    HANDLE h = CreateThread(NULL, 0, CleanBinThreadProc, NULL, 0, NULL);
    if (h) {
        CloseHandle(h);
    } else {
        g_cleaning = 0;
        LogMsg(L"cleanbin: failed to start thread");
    }
}

static void StartClean(wchar_t drive)
{
    if (g_cleaning) {
        LogMsg(L"clean: already running, ignored");
        return;
    }
    g_cleaning = 1;
    HANDLE h = CreateThread(NULL, 0, CleanThreadProc, (LPVOID)(ULONG_PTR)drive, 0, NULL);
    if (h) {
        CloseHandle(h);
    } else {
        g_cleaning = 0;
        LogMsg(L"clean: failed to start thread");
    }
}

static void TrayShowMenu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;

    wchar_t head[128];
    _snwprintf(head, 127, L"MemTune   GPU %d%%   CPU %d%%",
               g_lastGpu >= 0 ? g_lastGpu : 0, g_lastCpu >= 0 ? g_lastCpu : 0);
    head[127] = 0;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head);

    wchar_t mem[128];
    _snwprintf(mem, 127, L"可用内存 %.1f GB   风扇 %d%%",
               (double)g_lastFreeGB, g_fanLevel >= 0 ? g_fanLevel : 0);
    mem[127] = 0;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, mem);

    const wchar_t* modeText = (g_lastMode == 1) ? L"模式：优化中"
                            : (g_lastMode == 2) ? L"模式：已暂停"
                            : L"模式：空闲待机";
    wchar_t mbuf[128];
    _snwprintf(mbuf, 127, L"%s%s%s", modeText,
               g_lastGame[0] ? L"  " : L"", g_lastGame);
    mbuf[127] = 0;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, mbuf);

    /* 三角洲状态：局内会限流抖音（用主循环缓存的值，不重复采样）*/
    if (g_lastDfRun) {
        wchar_t dbuf[128];
        _snwprintf(dbuf, 127, L"三角洲：%s（CPU %d%%）",
                   g_lastDfInGame ? L"局内 - 已限制抖音" : L"局外 - 不限抖音",
                   g_lastDfCpu);
        dbuf[127] = 0;
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, dbuf);
    }

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_TRIMNOW, L"立即优化内存");

    /* 磁盘清理子菜单 */
    HMENU clean = CreatePopupMenu();
    if (clean) {
        AppendMenuW(clean, MF_STRING, IDM_CLEAN,     L"清理全部（C + D）");
        AppendMenuW(clean, MF_SEPARATOR, 0, NULL);
        AppendMenuW(clean, MF_STRING, IDM_CLEAN_C,   L"只清理 C 盘");
        AppendMenuW(clean, MF_STRING, IDM_CLEAN_D,   L"只清理 D 盘");
        AppendMenuW(clean, MF_SEPARATOR, 0, NULL);
        AppendMenuW(clean, MF_STRING, IDM_CLEAN_BIN, L"只清空回收站");
        AppendMenuW(clean, MF_SEPARATOR, 0, NULL);
        AppendMenuW(clean, MF_STRING | MF_GRAYED, 0, L"临时文件/缓存/回收站");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)clean, L"清理磁盘垃圾");
    }

    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE,
                g_paused ? L"恢复优化" : L"暂停优化");

    AppendMenuW(m, MF_STRING | (g_bgEco ? MF_CHECKED : 0), IDM_BG_ECO,
                L"后台进程降级（省 CPU/IO）");

    AppendMenuW(m, MF_STRING | (g_leakAuto ? MF_CHECKED : 0), IDM_LEAK_AUTO,
                L"内存泄漏自动处理");

    /* 内存占用排行子菜单 */
    {
        MemTopItem top[MEMTOP_MAX];
        int n = GetMemTop(top, 12);
        HMENU mm = CreatePopupMenu();
        if (mm) {
            int shown = 0;
            for (int i = 0; i < n; ++i) {
                if (top[i].wsMB < 20) break;   /* 小于 20MB 的不列 */
                wchar_t lb[96];
                _snwprintf(lb, 95, L"%-24s %llu MB", top[i].name, top[i].wsMB);
                lb[95] = 0;
                AppendMenuW(mm, MF_STRING, IDM_MEMTOP_BASE + shown, lb);
                /* 快照：点菜单时靠 id 回溯进程名 */
                g_memTopSnap[shown] = top[i];
                shown++;
                if (shown >= 10) break;
            }
            g_memTopSnapN = shown;
            if (shown == 0)
                AppendMenuW(mm, MF_STRING | MF_GRAYED, 0, L"（无）");
            AppendMenuW(mm, MF_SEPARATOR, 0, NULL);
            AppendMenuW(mm, MF_STRING | MF_GRAYED, 0, L"点进程名可立即优化它");
            wchar_t mtip[64];
            _snwprintf(mtip, 63, L"可用 %.1f GB / 共 15.1 GB", (double)g_lastFreeGB);
            mtip[63] = 0;
            AppendMenuW(mm, MF_STRING | MF_GRAYED, 0, mtip);
            AppendMenuW(m, MF_POPUP, (UINT_PTR)mm, L"内存占用排行");
        }
    }

    /* 风扇转速：手动档位 + 自动 */
    {
        static const int levels[] = { 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80, 90, 100 };
        HMENU fan = CreatePopupMenu();
        if (fan) {
            wchar_t hdr[96];
            if (g_fanManualOff)
                _snwprintf(hdr, 95, L"当前：手动 %d%%", g_fanLevel >= 0 ? g_fanLevel : 0);
            else
                _snwprintf(hdr, 95, L"当前：自动 %d%%", g_fanLevel >= 0 ? g_fanLevel : 0);
            hdr[95] = 0;
            AppendMenuW(fan, MF_STRING | MF_GRAYED, 0, hdr);
            AppendMenuW(fan, MF_SEPARATOR, 0, NULL);

            int nLev = (int)(sizeof(levels) / sizeof(levels[0]));
            for (int i = 0; i < nLev; ++i) {
                wchar_t lb[32];
                _snwprintf(lb, 31, L"%d%%", levels[i]);
                lb[31] = 0;
                UINT flags = MF_STRING;
                if (g_fanManualOff && g_fanLevel == levels[i]) flags |= MF_CHECKED;
                AppendMenuW(fan, flags, IDM_FAN_BASE + levels[i], lb);
            }

            AppendMenuW(fan, MF_SEPARATOR, 0, NULL);
            AppendMenuW(fan, MF_STRING | (g_fanManualOff ? 0 : MF_CHECKED),
                        IDM_FAN_AUTO, L"自动调速（跟 GPU/CPU）");

            wchar_t top[96];
            if (g_fanManualOff) _snwprintf(top, 95, L"风扇转速（手动 %d%%）", g_fanLevel);
            else                _snwprintf(top, 95, L"风扇转速（自动）");
            top[95] = 0;
            AppendMenuW(m, MF_POPUP, (UINT_PTR)fan, top);
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_OPEN_LOG, L"打开日志");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出 MemTune");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);       /* 否则菜单点外面不消失 */

    /* DPI 说明：m.xml 已声明 PerMonitorV2，系统会用真实 DPI 渲染菜单，
       不需要手动改字体（手动改反而导致字号不匹配系统）。 */
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void DoTrimNow(void)
{
    double before = GetFreeGB();
    TrimResult r = TrimProcsMulti(IDLE_TRIM_ABOVE_MB, 0, 2);
    double after = GetFreeGB();
    LogMsg(L"manual trim: trim=%d freed=%lluMB free=%.1f->%.1fGB",
           r.trimmed, r.freedMB, before, after);
    g_lastFreeGB = after;
}

static LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            TrayShowMenu(hwnd);
            return 0;
        }
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {   /* 双击 = 立即优化 */
            g_trimNow = 1;
            return 0;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TRIMNOW:    g_trimNow = 1; break;
        case IDM_CLEAN:      StartClean(0); break;   /* 0 = C+D 全部 */
        case IDM_CLEAN_C:    StartClean(L'C'); break;
        case IDM_CLEAN_D:    StartClean(L'D'); break;
        case IDM_CLEAN_BIN:  StartCleanBin(); break;
        case IDM_OPEN_LOG: {
                                 wchar_t p[MAX_PATH + 32];
                                 _snwprintf(p, MAX_PATH + 31, L"%s\\memtune.log", g_root);
                                 p[MAX_PATH + 31] = 0;
                                 ShellExecuteW(NULL, L"open", L"notepad.exe", p, NULL, SW_SHOWNORMAL);
                             }
                             break;
        case IDM_PAUSE:      g_paused = !g_paused;
                             LogMsg(L"tray: %s", g_paused ? L"paused" : L"resumed");
                             break;
        case IDM_BG_ECO:
                             g_bgEco = !g_bgEco;
                             LogMsg(L"tray: background eco %s", g_bgEco ? L"on" : L"off");
                             if (!g_bgEco) {
                                 /* 关掉时把降过的进程全部还原，不留尾巴 */
                                 int n = RestoreAllBackground();
                                 LogMsg(L"bg eco: restored %d processes", n);
                             }
                             break;
        case IDM_LEAK_AUTO:
                             g_leakAuto = !g_leakAuto;
                             LogMsg(L"tray: leak auto-handle %s", g_leakAuto ? L"on" : L"off");
                             break;
        case IDM_FAN_AUTO:
                             g_fanManualOff = 0;
                             g_fanLevel = -1;
                             g_fanLastChange = 0;
                             LogMsg(L"tray: fan auto control resumed");
                             break;
        case IDM_EXIT:       g_quit = 1; break;
        default: {
                             UINT id = LOWORD(wp);

                             /* 内存排行：点了就优化那个进程 */
                             if (id >= IDM_MEMTOP_BASE && id < IDM_MEMTOP_BASE + MEMTOP_MAX) {
                                 int idx = (int)(id - IDM_MEMTOP_BASE);
                                 if (idx < g_memTopSnapN) {
                                     wchar_t nm[64];
                                     wcsncpy(nm, g_memTopSnap[idx].name, 63);
                                     nm[63] = 0;
                                     int cnt = TrimByProcName(nm, 2);
                                     LogMsg(L"tray: trim '%s' -> %d proc", nm, cnt);
                                 }
                                 break;
                             }

                             if (id > IDM_FAN_BASE && id <= IDM_FAN_BASE + 100) {
                                 int lv = (int)(id - IDM_FAN_BASE);
                                 if (lv < 0) lv = 0;
                                 if (lv > 100) lv = 100;
                                 g_fanManualOff = 1;   /* 手动模式：停自动调速 */
                                 g_fanLastChange = GetTickCount();
                                 FanSend(lv);
                                 LogMsg(L"tray: fan manual -> %d%%", lv);
                             }
                         }
                         break;
        }
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 建隐藏窗口 + 托盘图标。失败也继续运行（只是没图标） */
/* 取当前 DPI。Win10+ 用 GetDpiForWindow，老系统回退 GetDeviceCaps */
static UINT GetDpiForTray(HWND hwnd)
{
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE u = LoadLibraryW(L"user32.dll");
        if (u) pfn = (PFN_GetDpiForWindow)GetProcAddress(u, "GetDpiForWindow");
    }
    if (pfn && hwnd) {
        UINT d = pfn(hwnd);
        if (d >= 72) return d;
    }
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return (d >= 72) ? (UINT)d : 96;
}

/* 按 DPI 选合适物理像素尺寸的图标。
   托盘小图标在 150% 缩放下需要 24px、200% 需要 32px、
   直接给实际像素尺寸才清晰，交给系统缩放必糊。 */
static HICON LoadTrayIcon(HINSTANCE hi, UINT dpi)
{
    int px = (int)(16.0 * dpi / 96.0 + 0.5);   /* 16px @96dpi 是托盘基准 */
    if (px < 16) px = 16;
    if (px <= 16)      px = 16;
    else if (px <= 20) px = 20;
    else if (px <= 24) px = 24;
    else if (px <= 32) px = 32;
    else if (px <= 48) px = 48;
    else if (px <= 64) px = 64;
    else               px = 128;

    /* 先用精确像素尺寸（LR_DEFAULTCOLOR 不缩放） */
    HICON ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 px, px, LR_DEFAULTCOLOR);
    if (!ic) ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!ic) ic = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    return ic;
}

static void TrayInit(HINSTANCE hi)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = TrayProc;
    wc.hInstance     = hi;
    wc.lpszClassName = TIP_CLASS;
    if (!RegisterClassW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    }

    g_trayHwnd = CreateWindowExW(0, TIP_CLASS, L"MemTune", 0,
                                 0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!g_trayHwnd) return;

    g_icon = LoadTrayIcon(hi, GetDpiForTray(g_trayHwnd));

    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_trayHwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = g_icon;
    wcsncpy(nid.szTip, L"MemTune - 内存优化器", 127);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void TraySetTip(const wchar_t* tip)
{
    if (!g_trayHwnd) return;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_trayHwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy(nid.szTip, tip, 127);
    nid.szTip[127] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayUpdateTip(void)
{
    wchar_t tip[128];
    const wchar_t* m = (g_lastMode == 1) ? L"优化中"
                     : (g_lastMode == 2) ? L"已暂停" : L"空闲";
    _snwprintf(tip, 127, L"MemTune\n%s  风扇%d%%  可用%.1fGB\nGPU %d%%  CPU %d%%",
               m, g_fanLevel >= 0 ? g_fanLevel : 0, (double)g_lastFreeGB,
               g_lastGpu >= 0 ? g_lastGpu : 0, g_lastCpu >= 0 ? g_lastCpu : 0);
    tip[127] = 0;
    TraySetTip(tip);
}

static void TrayDone(void)
{
    if (!g_trayHwnd) return;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_trayHwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(g_trayHwnd);
    g_trayHwnd = NULL;
}

/* 可中断的等待：每 100ms 泵一次窗口消息，顺便处理托盘指令。
   返回 1 表示要立刻跳出当前等待（GPU 变热等），0 表示正常走完。 */
static int PumpSleep(DWORD totalMs, int* outWaitedSec)
{
    DWORD step = 100, done = 0;
    while (done < totalMs) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) return 1;
        if (g_trimNow) {
            g_trimNow = 0;
            DoTrimNow();
            TrayUpdateTip();
        }
        Sleep(step);
        done += step;
    }
    if (outWaitedSec) *outWaitedSec = (int)(done / 1000);
    return 0;
}

// ==================== 单实例 ====================
static HANDLE g_mutex = NULL;

static int AlreadyRunning(void) {
    g_mutex = CreateMutexW(NULL, FALSE, L"Global\\MemTune_SingleInstance");
    if (!g_mutex) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    return 0;
}

// ==================== 入口 ====================
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hi; (void)hp; (void)cmd; (void)show;

    // 目录
    GetModuleFileNameW(NULL, g_root, MAX_PATH);
    wchar_t* slash = wcsrchr(g_root, L'\\');
    if (slash) *slash = 0;

    if (AlreadyRunning()) return 0;

    TrayInit(hi);        /* 右下角托盘图标 */

    LoadGames();
    g_lastFreeGB = GetFreeGB();
    TrayUpdateTip();
    LogMsg(L"=== MemTune start (pid=%lu, games=%d) ===", GetCurrentProcessId(), g_gameCount);

    /* 启动时探一次 GPU 计数器，日志里留个底，方便确认风扇策略走哪条 */
    {
        if (GetGpuUsage() < 0) Sleep(1000);   /* 首调只建基线，稍等才有值 */
        int g0 = GetGpuUsage();
        if (g0 < 0) LogMsg(L"GPU counter unavailable -> fan falls back to CPU");
        else        LogMsg(L"GPU counter OK (now=%d%%)", g0);
    }

    /* 启动时按当前 GPU 状态同步一次电源方案 */
    {
        int g0 = GetGpuUsage();   /* 首次调用可能只建基线，返回 -1 视为不热 */
        if (g0 >= GPU_HOT_PCT) ApplyPowerScheme(PWR_GAME, 1, 1);
        else                   ApplyPowerScheme(PWR_QUIET, 0, 1);
    }

    wchar_t mode[16] = L"";
    int hotStreak = 0;
    unsigned long long round = 0;
    DWORD startTick = GetTickCount();

    /* ============================================================
       调度原则：一律由 GPU 占用决定，不再依赖「是否检测到游戏」。
         GPU >= 80%  -> 全量优化（深度 trim 所有进程 + 清缓存 + 关多余进程）
         GPU <  80%  -> 完全不优化，只更新风扇和内存占用回报
       这样浏览器看 4K、剪映渲染、跑 AI 之类吃 GPU 的场景也会自动进入优化，
       不必先被 games.txt 认出来。
       ============================================================ */
    for (;;) {
        if (g_quit) break;
        round++;
        LoadGames();

        int gpu = GetGpuUsage();
        int cpu = GetCpuUsage();
        int gpuHot = (gpu >= 0 && gpu >= GPU_HOT_PCT);

        /* 发布状态给托盘 */
        g_lastGpu = gpu;
        g_lastCpu = cpu;

        /* 风扇永远跟着 GPU/CPU 走，与是否优化无关 */
        ApplyFan(DecideFanByLoad(gpu, cpu, gpuHot), 0);

        /* 三角洲局内/局外检测：局内才压抖音，局外一律不管
           （刷抖音时不动，切回游戏自动限流，无需关进程）*/
        int dfInGame = UpdateDeltaForceState(gpu);
        g_lastDfInGame = dfInGame;

        /* 游戏启动预优化：任何 games.txt 里的游戏一启动就腾内存，不只三角洲。
           GPU 热的轮次不做（说明已经进去了，正在玩，不折腾）。*/
        {
            static wchar_t s_lastGame[64] = L"";
            wchar_t curGame[64] = L"";
            int anyGame = DetectGameAuto(curGame);
            if (anyGame && !gpuHot) GamePreOptTick(curGame);
            /* 游戏换了或者退了，清掉旧状态，下次启动能重做 */
            if (s_lastGame[0] && (!anyGame || wcscmp(s_lastGame, curGame) != 0)) {
                GamePreOptReset(s_lastGame);
                s_lastGame[0] = 0;
            }
            if (anyGame) { wcsncpy(s_lastGame, curGame, 63); s_lastGame[63] = 0; }
        }

        /* 后台进程降级：每 120 秒刷一次。
           降级是幂等的，没必要频繁做（每次都要开一堆句柄，反而费 CPU）。 */
        if (g_bgEco && !g_paused) {
            static DWORD s_lastEco = 0;
            DWORD tNow = GetTickCount();
            if (s_lastEco == 0 || (tNow - s_lastEco) > 120000) {
                s_lastEco = tNow;
                int n = ApplyBackgroundEco();
                LogMsg(L"bg eco: %d processes marked", n);
                g_ecoCount = n;
            }
        }

        /* 无感监控：内存体检 / 网络延迟 / 定时自动优化（内部自己控频率）*/
        {
            static DWORD s_lastHealth = 0;
            DWORD tNow2 = GetTickCount();
            if (s_lastHealth == 0 || (tNow2 - s_lastHealth) > 600000) {
                s_lastHealth = tNow2;
                LogMemHealth();
            }
        }
        NetCheck();
        if (!g_paused) AutoTrimTick();
        LeakTick();   /* 内存泄漏检测：每 60 秒采样一次，自己控频率 */

        /* 托盘「暂停优化」：只停内存清理，风扇照常 */
        if (g_paused) {
            g_lastMode = 2;
            g_lastFreeGB = GetFreeGB();
            TrayUpdateTip();
            StallCheck(3000);
            PumpSleep(3000, NULL);
            continue;
        }

        if (gpuHot) {
            wchar_t game[64] = L"";
            int isGame = DetectGameAuto(game);   /* 仅用于日志 */

            /* 连续计数：GPU 要连热 HOT_CONFIRM 轮才算真的热，
               防止桌面渲染 / 网页视频 / 编译时 GPU 尖峰误触发全量清理。
               只做 trim，不再 CloseUnneededProcesses —— 那条路会误杀前台程序。 */
            hotStreak++;
            g_lastMode = 1;
            if (game[0]) { wcsncpy(g_lastGame, game, 63); g_lastGame[63] = 0; }
            if (wcscmp(mode, L"hot") != 0) {
                LogMsg(L"MODE -> hot (gpu=%d%%%s%s)", gpu,
                       isGame ? L", game=" : L"", isGame ? game : L"");
                wcscpy(mode, L"hot");
                ApplyPowerScheme(PWR_GAME, 1, 0);
                /* 绝不在这里 CloseUnneededProcesses：会把前台工作程序一起杀。 */
            }

            double freeGB = GetFreeGB();
            TrimResult r = TrimProcsMulti(GAME_TRIM_ABOVE_MB, 1, GAME_TRIM_REPEAT);

            int purged = 0;
            if (freeGB < GAME_PURGE_BELOW_GB || freeGB < GAME_HARD_PURGE_GB) {
                PurgeSystem(4);   /* 清 standby 缓存列表 */
                PurgeSystem(3);   /* flush 脏页到页面文件 */
                PurgeSystem(4);   /* 再清一次，把刚 flush 下来的也赶走 */
                purged = 1;
            }

            double after = GetFreeGB();
            g_lastFreeGB = after;
            g_lastGpu = gpu;
            g_lastCpu = cpu;
            TrayUpdateTip();
            LogMsg(L"hot pass%llu gpu=%d%% trim=%d freed=%lluMB standby=%s free=%.1f->%.1fGB",
                   round, gpu, r.trimmed, r.freedMB, purged ? L"purged" : L"-", freeGB, after);

            SlimSelf();

            /* 分段等待：每秒看 GPU 是否已回落，及时停止优化 */
            {
                int waited = 0;
                while (waited < GAME_INTERVAL_SEC) {
                    if (PumpSleep(1000, NULL)) break;      /* 退出指令 */
                    waited++;
                    StallCheck(1000);
                    int g2 = -1, c2 = 0;
                    if ((waited % 5) == 0) {   /* 5 秒查一次，别太频繁 */
                        g2 = GetGpuUsage();
                        c2 = GetCpuUsage();
                        g_lastGpu = g2; g_lastCpu = c2;
                        ApplyFan(DecideFanByLoad(SmoothLoad(&s_gpuSm, g2), SmoothLoad(&s_cpuSm, c2), g2 >= GPU_HOT_PCT), 0);
                        TrayUpdateTip();
                        if (!(g2 >= 0 && g2 >= GPU_HOT_PCT)) break;   /* GPU 回落，退出热模式 */
                    }
                }
            }
        }
        else {
            hotStreak = 0;
            g_lastMode = 0;
            if (wcscmp(mode, L"idle") != 0) {
                LogMsg(L"MODE -> idle (gpu=%d%%)", gpu);
                wcscpy(mode, L"idle");
                ApplyPowerScheme(PWR_QUIET, 0, 0);  /* 切回安静电源方案 */
                RestoreAfterGame();                 /* 恢复截屏工具 */
            }

            SlimSelf();

            /* 分段等待：每秒看 GPU 是否冲上 80%，一旦变热立刻回主循环 */
            {
                int interval = IDLE_INTERVAL_SEC;
                int inGrace = ((GetTickCount() - startTick) < (DWORD)(STARTUP_GRACE_SEC * 1000));
                if (inGrace) interval = 10;   /* 开机宽限期内先别干别的 */

                int waited = 0;
                while (waited < interval) {
                    if (PumpSleep(1000, NULL)) break;      /* 退出指令 */
                    waited++;
                    StallCheck(1000);

                    int g2 = GetGpuUsage();
                    int c2 = GetCpuUsage();
                    g_lastGpu = g2; g_lastCpu = c2;
                    g_lastFreeGB = GetFreeGB();
                    ApplyFan(DecideFanByLoad(SmoothLoad(&s_gpuSm, g2), SmoothLoad(&s_cpuSm, c2), 0), 0);
                    if ((waited % 10) == 0) TrayUpdateTip();

                    /* GPU 冲上去了：立刻跳出，回主循环进热模式 */
                    if (g2 >= 0 && g2 >= GPU_HOT_PCT) break;
                }
            }
        }
    }

    /* 退出：把风扇交还 BIOS，删托盘图标 */
    LogMsg(L"=== MemTune exit ===");
    FanCommand("DEFAULT\n", NULL, 0);
    TrayDone();
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); }
return 0;
}
