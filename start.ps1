param([switch]$LoadModel, [switch]$Development)
$ErrorActionPreference = 'Stop'
# 优先启动完整分发；开发版只为当前进程准备 Qt 动态库路径。
$taskPackaged = Join-Path $PSScriptRoot 'dist/ModelObservatoryCpp/Xiyuan.exe'
$taskDevelopment = Join-Path $PSScriptRoot 'work/client-cpp/Xiyuan.exe'
$taskArguments = @()
if ($LoadModel) { $taskArguments += '--load' }
# 显式开发入口用于验收最新编译结果，避免误启动旧版完整分发目录。
if (-not $Development -and (Test-Path -LiteralPath $taskPackaged)) {
    Start-Process -FilePath $taskPackaged -ArgumentList ($taskArguments + '--mode observe')
    return
}
if (-not (Test-Path -LiteralPath $taskDevelopment)) { throw 'C++ 客户端尚未构建，请运行 build.ps1。' }
$taskPreviousPath = $env:PATH
$taskPreviousPlugin = $env:QT_PLUGIN_PATH
try {
    $env:PATH = "$PSScriptRoot/.cache/qt/6.8.3/mingw_64/bin;$PSScriptRoot/.cache/qt/Tools/mingw1310_64/bin;$taskPreviousPath"
    $env:QT_PLUGIN_PATH = Join-Path $PSScriptRoot '.cache/qt/6.8.3/mingw_64/plugins'
    Start-Process -FilePath $taskDevelopment -ArgumentList ($taskArguments + '--mode observe')
}
finally { $env:PATH = $taskPreviousPath; $env:QT_PLUGIN_PATH = $taskPreviousPlugin }
