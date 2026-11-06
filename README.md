# MemTune

Windows 小工具：**内存优化** + **IT87 系列 SuperIO 芯片的风扇调速**。托盘常驻，按 GPU / CPU 负载自动升降风扇档位，空闲时整理内存工作集、清理待机缓存。

> 硬件实测环境：Gigabyte B650M D2HP（ITE IT8689E），AMD Ryzen 5 9600X + RTX 5060 Ti。
> IT87 系列（IT8686E / IT8688E / IT8689E / IT8625E 等）理论上通用，但 LHM 的主板型号表里可能没有你的板子，本程序是**直接按端口探测 IT87**，所以不依赖主板表。

## 功能

- **风扇控制**：读 GPU 占用率决定档位（<20% 静音 25%、20–50% 35%、50–80% 55%、≥80% 或游戏内 100%），带指数平滑 + 10% 死区 + 15 秒最小换档间隔，避免风扇反复变速的呼呼声
- **内存优化**：按阈值清理进程工作集、清 standby 缓存；有硬保护名单，不会动 explorer、输入法、音频、显卡驱动等关键进程
- **电源模式**：热/闲自动在「高性能 / 平衡」之间切换（可在托盘关闭）
- **无感监控**：内存体检、网络延迟检测、内存泄漏预警，全部只在托盘提示，不弹窗打扰

## 系统要求

| 项目 | 要求 |
|---|---|
| 系统 | Windows 10 1809+ / Windows 11，x64 |
| 权限 | 管理员（读写 SuperIO 端口必需） |
| 驱动 | **PawnIO**（安装包已自带，风扇控制依赖它做端口 I/O） |
| 运行库 | .NET Framework 4.8（Win10/11 自带） |

## 安装

**推荐：单文件安装包** `MemTune-Setup-1.0.0.exe`。双击就能装，不用解压、不用联网——所有依赖和 PawnIO 安装器都内嵌在 exe 里（约 4.2 MB）。

1. 双击 exe，UAC 弹窗选「是」（风扇控制必须管理员权限）
2. 安装器自动完成：解出全部文件 → 复制到 `C:\Program Files\MemTune` → 按需安装 PawnIO 驱动 → 建桌面/开始菜单快捷方式 → 注册卸载项 → 跑一次风扇自检并提示识别到的通道数
3. 装完从桌面图标启动即可

卸载：设置 → 应用 → MemTune → 卸载，或运行安装目录里的 `Uninstall.ps1`。
卸载默认**保留 PawnIO**（其他硬件工具也用它），要一起删就加参数 `-RemovePawnIO`。

> 静默安装：`MemTune-Setup-1.0.0.exe /silent /dir="D:\Apps\MemTune"`
> 静默卸载：`MemTune-Setup-1.0.0.exe /silent /uninstall`
> 没有代码签名证书，首次运行可能弹 SmartScreen，点「更多信息 → 仍要运行」。
> 备用方案是 Release 里的 `MemTune-Setup.zip`（绿色版，解压后双击 `Setup.cmd`，效果等价）。

### 手动安装

```
1. 把 app\ 里的全部文件放到同一目录
2. 安装 PawnIO：third_party\PawnIO_setup.exe -install
3. 启动 MemTune.exe（必须管理员）
```

## 目录结构

> 下面是 **Release 里的安装包 / 绿色包** 内部结构。本仓库只放源码和脚本：`app/`（编译好的程序）、`third_party/`（PawnIO 安装器）、`dist/`（安装包）都是构建产物，只随 Release 分发，不进仓库。

```
MemTune-Setup/
├── Setup.cmd                 绿色版双击安装（自动提权）
├── Install.ps1               安装逻辑（安装器与绿色版共用）
├── Uninstall.ps1             卸载逻辑
├── app/                      程序本体 + 全部依赖
│   ├── MemTune.exe           主程序（内存优化 / 托盘）
│   ├── FanBridge.exe         风扇控制宿主（常驻，命名管道服务端）
│   ├── FanBridge.dll         风扇控制实现（反射调用 LHM）
│   ├── LibreHardwareMonitorLib.dll
│   ├── HidSharp.dll、System.*.dll、RAMSPDToolkit-NDD.dll
├── third_party/
│   └── PawnIO_setup.exe      驱动安装器（原版未修改）
├── tools/                    单文件安装器的源码与构建脚本
│   ├── Installer.cs          安装器主体（解包 → 调 Install.ps1 → 显示进度）
│   ├── admin.manifest        强制管理员权限
│   └── build-installer.ps1   一键重新打包成 exe
└── dist/                     构建产物（MemTune-Setup-1.0.0.exe）
```

## 工作原理

```
MemTune.exe ──命名管道 MemTuneFanCtl──> FanBridge.exe ──反射──> LibreHardwareMonitorLib
                                                                    │
                                                              LpcIO(端口 0x2E/0x4E)
                                                                    │
                                                                PawnIO.sys ──> IT87 芯片
```

- `FanBridge.exe` 必须常驻且**必须管理员**：端口 I/O 无权限时 `DetectIT87` 直接返回 false，`MemTune.exe` 通过 `RUNASADMIN` 标记保证整条进程链都是提权的
- 命名管道协议（UTF-8，一行一条）：
  - `SET <0-100>` → `OK <通道数>`
  - `DEFAULT` → `OK`（交还 BIOS 自动控制）
  - `FANS` / `TEMPS` → `OK <逗号分隔数值>`
  - `QUIT` → `BYE`

## 常见问题

**日志出现 `init failed: DetectIT87 failed (need admin?)`**
九成不是权限问题，是 PawnIO 驱动没装。确认方式：`Get-Service PawnIO` 为空、`C:\Program Files\PawnIO` 不存在。重装 PawnIO 即可。
另外，如果安装器提示 "A previous installation of PawnIO was found"，是注册表残留登记，先跑一次 `PawnIO_setup.exe -uninstall -silent` 再 `-install -silent`。

**风扇通道数是 0 或比实际少**
不同主板的 IT87 通道数不同，本程序按芯片实际寄存器读，每个 IT87XX 实例 6 路。若你的板子有 2 颗芯片，应为 12 路。

**日志出现 `power switch FAILED (code=1)`**
`powercfg -setactive <GUID>` 失败，通常不是权限，而是程序里写死的电源方案 GUID 在你这台机器上不存在。用 `powercfg -list` 对照，把 `MemTune.c` 里的 `PWR_GAME` / `PWR_QUIET` 换成你系统里真实存在的 GUID。

**开机后风扇一直全速**
程序异常退出时没执行 `DEFAULT`，风扇停在最后一次设定值。重开一次 MemTune 后从托盘选「交还 BIOS」即可。

## 从源码构建

需要 **mingw-w64 gcc**（含 windres）和 .NET Framework 的 `csc.exe`：

```bat
:: 1) 改 src\build.bat 里的 PATH（指向你的 mingw/bin 和 csc 路径）后运行
src\build.bat
```

产物：`MemTune.exe`（C，-mwindows，资源含图标与 DPI 清单）、`FanBridge.dll`、`FanBridge.exe`（C#）。
把它们和依赖 DLL 一起放进 `app\`，再跑 `tools\build-installer.ps1` 就能重新生成单文件安装包（只用 Windows 自带的 csc，不需要额外下载工具）。

## 许可证

- 本项目源码：MIT（见 `LICENSE`，发布前把版权人改成你自己）
- 第三方组件见 `THIRD_PARTY_NOTICES.md`

## 免责声明

风扇转速与内存清理都直接作用于硬件/系统状态，使用风险自负。程序对关键进程有硬保护名单，但**仍建议先在一台非关键机器上试用**。
