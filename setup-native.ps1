$ErrorActionPreference = 'Stop'
# 工具链仅安装到项目缓存，不修改全局 PATH 或系统编译器。
$taskPython = Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $taskPython)) { & (Join-Path $PSScriptRoot 'setup.ps1') }
& $taskPython -m pip install aqtinstall==3.3.0 cmake==4.4.4 ninja==1.13.2
if ($LASTEXITCODE -ne 0) { throw '构建依赖安装失败。' }
$taskQtRoot = Join-Path $PSScriptRoot '.cache/qt'
if (-not (Test-Path -LiteralPath (Join-Path $taskQtRoot '6.8.3/mingw_64/lib/cmake/Qt6/Qt6Config.cmake'))) {
    & $taskPython -m aqt install-qt windows desktop 6.8.3 win64_mingw -O $taskQtRoot --archives qtbase qttools
    if ($LASTEXITCODE -ne 0) { throw 'Qt SDK 安装失败。' }
}
if (-not (Test-Path -LiteralPath (Join-Path $taskQtRoot 'Tools/mingw1310_64/bin/g++.exe'))) {
    & $taskPython -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 -O $taskQtRoot
    if ($LASTEXITCODE -ne 0) { throw 'C++ 编译器安装失败。' }
}
Write-Host 'C++ 构建环境已就绪。'
