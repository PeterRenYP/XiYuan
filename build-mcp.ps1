param()
$ErrorActionPreference = 'Stop'
# MCP 客户端使用独立构建与输出目录，不替换其他会话正在打包的客户端。
$taskRoot = $PSScriptRoot
$taskCMake = Join-Path $taskRoot '.venv/Scripts/cmake.exe'
$taskQt = Join-Path $taskRoot '.cache/qt/6.8.3/mingw_64'
$taskCompiler = Join-Path $taskRoot '.cache/qt/Tools/mingw1310_64/bin'
$taskBuild = Join-Path $taskRoot 'work/mcp-release'
$taskOutput = Join-Path $taskRoot 'dist/ModelObservatoryMcp'
$taskPreviousPath = $env:PATH
try {
    $env:PATH = "$taskCompiler;$taskQt/bin;$taskPreviousPath"
    & $taskCMake -S (Join-Path $taskRoot 'client-cpp') -B $taskBuild -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_PREFIX_PATH=$taskQt" "-DCMAKE_CXX_COMPILER=$taskCompiler/g++.exe" `
        "-DCMAKE_MAKE_PROGRAM=$taskRoot/.venv/Scripts/ninja.exe"
    if ($LASTEXITCODE -ne 0) { throw 'MCP 客户端配置失败。' }
    & $taskCMake --build $taskBuild -j 3
    if ($LASTEXITCODE -ne 0) { throw 'MCP 客户端编译失败。' }
    New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
    Copy-Item -LiteralPath "$taskBuild/Xiyuan.exe" -Destination $taskOutput -Force
    & "$taskQt/bin/windeployqt.exe" --release --compiler-runtime --no-translations --no-opengl-sw `
        --no-system-d3d-compiler "$taskOutput/Xiyuan.exe"
    if ($LASTEXITCODE -ne 0) { throw 'MCP 客户端依赖部署失败。' }
    Write-Host "构建完成：$taskOutput/Xiyuan.exe"
    Write-Host '此本机版本从项目 .venv 启动推理运行器，请保留项目目录和 Python 环境。'
}
finally {
    $env:PATH = $taskPreviousPath
}
