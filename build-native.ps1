param([switch]$SkipRuntime)
$ErrorActionPreference = 'Stop'
$taskPython = Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
$taskQt = Join-Path $PSScriptRoot '.cache/qt/6.8.3/mingw_64'
$taskCompiler = Join-Path $PSScriptRoot '.cache/qt/Tools/mingw1310_64/bin'
if (-not (Test-Path -LiteralPath (Join-Path $taskCompiler 'g++.exe')) -or
    -not (Test-Path -LiteralPath (Join-Path $taskQt 'lib/cmake/Qt6/Qt6Config.cmake'))) {
    & (Join-Path $PSScriptRoot 'setup-native.ps1')
}
# PowerShell 与 Python 入口共用发布流程，完整构建自动打包运行器和 MCP。
$taskArguments = @((Join-Path $PSScriptRoot 'tools/release.py'))
if ($SkipRuntime) { $taskArguments += '--skip-runtime' }
& $taskPython @taskArguments
if ($LASTEXITCODE -ne 0) { throw '完整分发构建失败。' }
