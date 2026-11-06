<#
  MemTune 卸载脚本
  默认保留 PawnIO 驱动（其他硬件工具也在用）；加 -RemovePawnIO 才一并卸掉。
#>
[CmdletBinding()]
param(
    [switch]$RemovePawnIO,
    [switch]$Silent
)

$ErrorActionPreference = 'Continue'
$installDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($RemovePawnIO) { $argLine += ' -RemovePawnIO' }
    Start-Process -FilePath 'powershell.exe' -ArgumentList $argLine -Verb RunAs
    exit
}

if (-not $Silent) { Write-Host '=== 卸载 MemTune ===' -ForegroundColor White }

foreach ($n in 'MemTune', 'FanBridge') {
    Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 800

# 快捷方式
$links = @(
    (Join-Path $env:USERPROFILE 'Desktop\MemTune.lnk'),
    (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\MemTune.lnk')
)
foreach ($l in $links) { Remove-Item -LiteralPath $l -Force -ErrorAction SilentlyContinue }

# 管理员运行标记
$layers = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$exePath = Join-Path $installDir 'MemTune.exe'
if (Test-Path -LiteralPath $layers) {
    Remove-ItemProperty -Path $layers -Name $exePath -Force -ErrorAction SilentlyContinue
}

# 卸载登记
Remove-Item -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\MemTune' -Recurse -Force -ErrorAction SilentlyContinue

# PawnIO
if ($RemovePawnIO) {
    $setup = Join-Path $installDir 'third_party\PawnIO_setup.exe'
    if (Test-Path -LiteralPath $setup) { & $setup -uninstall -silent 2>&1 | Out-Null }
}

# 程序目录（脚本自身所在目录，删之前先退出该目录）
$logCopy = Join-Path $env:TEMP 'memtune_last_logs'
New-Item -ItemType Directory -Path $logCopy -Force | Out-Null
foreach ($lg in 'memtune.log', 'fanbridge.log') {
    $p = Join-Path $installDir $lg
    if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $logCopy -Force -ErrorAction SilentlyContinue }
}
Set-Location -LiteralPath $env:TEMP
Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue

if (-not $Silent) {
    Write-Host '卸载完成。' -ForegroundColor Green
    Write-Host "旧日志已备份到: $logCopy"
}
