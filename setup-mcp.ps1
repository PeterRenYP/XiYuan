param([switch]$Build)
$ErrorActionPreference = 'Stop'
# 依赖与推理环境分开，避免影响正在进行的模型运行或客户端打包。
$taskRoot = $PSScriptRoot
$taskPython = Join-Path $taskRoot '.venv/Scripts/python.exe'
$taskMcpPython = Join-Path $taskRoot '.venv-mcp/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $taskMcpPython)) {
    if (-not (Test-Path -LiteralPath $taskPython)) { throw '请先运行项目的 setup.ps1，准备 Python 环境。' }
    & $taskPython -m venv (Join-Path $taskRoot '.venv-mcp')
    if ($LASTEXITCODE -ne 0) { throw 'MCP 独立环境创建失败。' }
}
& $taskMcpPython -m pip install -r (Join-Path $taskRoot 'requirements-mcp.txt')
if ($LASTEXITCODE -ne 0) { throw 'MCP 依赖安装失败。' }
if ($Build) {
    # 独立服务在自己的环境中打包，避免把推理库带入 MCP 可执行文件。
    & $taskMcpPython -m pip install pyinstaller==6.22.3
    if ($LASTEXITCODE -ne 0) { throw 'MCP 打包依赖安装失败。' }
}
Write-Host 'MCP 环境已准备。启动支持 MCP 的模型观测台后，即可使用 run_mcp.py。'
