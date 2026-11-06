# 重新生成 MemTune-Setup-1.0.0.exe（单文件安装器）
# 依赖：Windows 自带的 .NET Framework csc.exe（不需要额外下载任何东西）
# 用法：在仓库根目录执行 powershell -ExecutionPolicy Bypass -File tools\build-installer.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$stageRoot = Join-Path $env:TEMP 'memtune_payload'
$stage = Join-Path $stageRoot 'MemTune-Setup'
$payload = Join-Path $root 'tools\payload.zip'
$outExe = Join-Path $root 'dist\MemTune-Setup-1.0.0.exe'

# 1) 收集要内嵌的文件
if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach ($item in 'Setup.cmd', 'Install.ps1', 'Uninstall.ps1', 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'app', 'src', 'third_party') {
    $src = Join-Path $root $item
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $stage -Recurse -Force }
}

# 2) 打载荷
Remove-Item -LiteralPath $payload -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $payload -CompressionLevel Optimal -Force

# 3) 编译单文件安装器（资源名固定为 payload.zip，Installer.cs 里按这个名字读）
New-Item -ItemType Directory -Path (Join-Path $root 'dist') -Force | Out-Null
Remove-Item -LiteralPath $outExe -Force -ErrorAction SilentlyContinue
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $csc /nologo /target:winexe /platform:anycpu /out:"$outExe" `
    /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll `
    /r:System.Windows.Forms.dll /r:System.Drawing.dll `
    /win32manifest:"$root\tools\admin.manifest" /win32icon:"$root\src\memtune.ico" `
    /resource:"$payload",payload.zip "$root\tools\Installer.cs"
if ($LASTEXITCODE -ne 0) { throw "csc 编译失败：$LASTEXITCODE" }

Remove-Item -LiteralPath $stageRoot -Recurse -Force -ErrorAction SilentlyContinue
$size = [math]::Round((Get-Item -LiteralPath $outExe).Length / 1MB, 2)
Write-Host "已生成 $outExe ($size MB)"
