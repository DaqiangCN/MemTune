<#
  MemTune 安装脚本
  - 复制程序与全部依赖到安装目录
  - 检查并按需安装 PawnIO 驱动（风扇端口 I/O 依赖）
  - 给主程序打上「始终以管理员身份运行」标记
  - 创建桌面 / 开始菜单快捷方式，注册卸载项
  - 最后跑一次风扇自检，报告识别到的通道数
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'MemTune'),
    [switch]$NoPawnIO
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Step($m) { Write-Host ''; Write-Host "==> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "    [OK] $m" -ForegroundColor Green }
function Note($m) { Write-Host "    [..] $m" }
function Warn($m) { Write-Host "    [!!] $m" -ForegroundColor Yellow }

# ---------- 1. 管理员权限 ----------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host '需要管理员权限，正在请求提权...'
    $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallDir `"$InstallDir`""
    if ($NoPawnIO) { $argLine += ' -NoPawnIO' }
    Start-Process -FilePath 'powershell.exe' -ArgumentList $argLine -Verb RunAs
    exit
}

$InstallDir = [IO.Path]::GetFullPath($InstallDir)

Write-Host '=== MemTune 安装程序 ===' -ForegroundColor White
Note "目标目录: $InstallDir"

# ---------- 2. 关闭正在运行的实例 ----------
Step '关闭正在运行的 MemTune / FanBridge'
foreach ($n in 'MemTune', 'FanBridge') {
    $ps = Get-Process -Name $n -ErrorAction SilentlyContinue
    if ($ps) { $ps | Stop-Process -Force -ErrorAction SilentlyContinue; Ok "已结束 $n" }
    else { Note "$n 未在运行" }
}
Start-Sleep -Milliseconds 800

# ---------- 3. 复制文件 ----------
Step '复制程序与依赖'
if (-not (Test-Path -LiteralPath $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}
$src = Join-Path $here 'app'
if (-not (Test-Path -LiteralPath $src)) { throw "安装包不完整：缺少 app 目录" }
$files = Get-ChildItem -LiteralPath $src -File
foreach ($f in $files) { Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $InstallDir $f.Name) -Force }
Ok "已复制 $($files.Count) 个文件"

foreach ($s in 'Install.ps1', 'Uninstall.ps1') {
    $p = Join-Path $here $s
    if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination (Join-Path $InstallDir $s) -Force }
}
$third = Join-Path $here 'third_party'
if (Test-Path -LiteralPath $third) {
    Copy-Item -LiteralPath $third -Destination $InstallDir -Recurse -Force
    Ok '已复制 PawnIO 安装器（便于今后离线重装驱动）'
}

# ---------- 4. PawnIO 驱动 ----------
if (-not $NoPawnIO) {
    Step '检查 PawnIO 驱动（风扇读写端口的底层通道）'
    $pawnOk = $false
    if (Get-Service -Name 'PawnIO' -ErrorAction SilentlyContinue) { $pawnOk = $true }
    if ($pawnOk) {
        Ok 'PawnIO 已安装'
    } else {
        $setup = Join-Path $here 'third_party\PawnIO_setup.exe'
        if (-not (Test-Path -LiteralPath $setup)) {
            Warn '安装包内缺少 PawnIO_setup.exe，跳过驱动安装，风扇控制将不可用'
        } else {
            Note '先清理可能残留的旧安装登记...'
            & $setup -uninstall -silent 2>&1 | Out-Null
            Note '正在静默安装 PawnIO...'
            & $setup -install -silent 2>&1 | Out-Null
            Start-Sleep -Seconds 2
            if (Get-Service -Name 'PawnIO' -ErrorAction SilentlyContinue) {
                $pawnOk = $true
                Ok 'PawnIO 安装成功'
            } else {
                Warn '静默安装未生效，改走图形界面安装，请在弹出的窗口点「确定」继续'
                Start-Process -FilePath $setup -ArgumentList '-install' -Wait
                Start-Sleep -Seconds 2
                if (Get-Service -Name 'PawnIO' -ErrorAction SilentlyContinue) {
                    $pawnOk = $true
                    Ok 'PawnIO 安装成功'
                } else {
                    Warn 'PawnIO 仍未安装成功：风扇控制不可用，可到 https://pawnio.eu/ 手动安装后重装本程序'
                }
            }
        }
    }
} else {
    Step '按参数要求跳过 PawnIO'
}

# ---------- 5. 始终以管理员身份运行 ----------
Step '设置主程序以管理员身份运行'
$exePath = Join-Path $InstallDir 'MemTune.exe'
$layers = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
if (-not (Test-Path -LiteralPath $layers)) { New-Item -Path $layers -Force | Out-Null }
New-ItemProperty -Path $layers -Name $exePath -Value '~ RUNASADMIN' -PropertyType String -Force | Out-Null
Ok '已设置 RUNASADMIN'

# ---------- 6. 快捷方式 ----------
Step '创建快捷方式'
$ws = New-Object -ComObject WScript.Shell
$links = @(
    (Join-Path $env:USERPROFILE 'Desktop\MemTune.lnk'),
    (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\MemTune.lnk')
)
foreach ($lnkPath in $links) {
    try {
        $lnk = $ws.CreateShortcut($lnkPath)
        $lnk.TargetPath = $exePath
        $lnk.WorkingDirectory = $InstallDir
        $lnk.IconLocation = "$exePath,0"
        $lnk.Description = 'MemTune 内存优化 + 风扇控制'
        $lnk.Save()
        Ok $lnkPath
    } catch { Warn "创建失败: $lnkPath ($($_.Exception.Message))" }
}

# ---------- 7. 卸载登记 ----------
Step '写入卸载注册项'
$unKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\MemTune'
if (Test-Path -LiteralPath $unKey) { Remove-Item -LiteralPath $unKey -Recurse -Force }
New-Item -Path $unKey -Force | Out-Null
$sizeKb = [int]((Get-ChildItem -LiteralPath $InstallDir -Recurse -File | Measure-Object Length -Sum).Sum / 1KB)
$props = @{
    DisplayName     = 'MemTune'
    DisplayVersion  = '1.0.0'
    Publisher       = 'MemTune'
    InstallLocation = $InstallDir
    DisplayIcon     = $exePath
    UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDir 'Uninstall.ps1')`""
    QuietUninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDir 'Uninstall.ps1')`" -Silent"
    NoModify        = 1
    NoRepair        = 1
    EstimatedSize   = $sizeKb
}
foreach ($k in $props.Keys) {
    if ($props[$k] -is [int]) { New-ItemProperty -Path $unKey -Name $k -Value $props[$k] -PropertyType DWord -Force | Out-Null }
    else { New-ItemProperty -Path $unKey -Name $k -Value $props[$k] -PropertyType String -Force | Out-Null }
}
Ok '已注册到「应用和功能」列表'

# ---------- 8. 自检 ----------
Step '风扇自检'
$bridge = Join-Path $InstallDir 'FanBridge.exe'
$tmpOut = Join-Path $env:TEMP 'memtune_fanselftest.txt'
try {
    $p = Start-Process -FilePath $bridge -ArgumentList '--test' -WorkingDirectory $InstallDir -RedirectStandardOutput $tmpOut -NoNewWindow -Wait -PassThru
    $out = if (Test-Path -LiteralPath $tmpOut) { (Get-Content -LiteralPath $tmpOut -Raw).Trim() } else { '' }
    if ($p.ExitCode -eq 0 -and $out -match 'OK\s+(\d+)') {
        Ok "风扇可用，识别到 $($Matches[1]) 个通道"
    } else {
        Warn "自检未通过（退出码 $($p.ExitCode)，输出: $out）"
    }
} catch {
    Warn "自检执行失败: $($_.Exception.Message)"
} finally {
    Remove-Item -LiteralPath $tmpOut -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host '=== 安装完成 ===' -ForegroundColor Green
Write-Host "安装目录: $InstallDir"
Write-Host '从桌面或开始菜单启动 MemTune 即可（首次会弹一次 UAC，之后一直用管理员运行）。'
