$ErrorActionPreference = 'Stop'
# 环境与下载缓存均留在项目磁盘，不修改系统 Python。
$taskEnvironment = Join-Path $PSScriptRoot '.venv'
if (-not (Test-Path -LiteralPath (Join-Path $taskEnvironment 'Scripts/python.exe'))) {
    & py -3.12 -m venv $taskEnvironment
    if ($LASTEXITCODE -ne 0) { throw '创建 Python 3.12 环境失败。' }
}
$taskPython = Join-Path $taskEnvironment 'Scripts/python.exe'
& $taskPython -m pip install --cache-dir (Join-Path $PSScriptRoot '.cache/pip') torch==2.9.1 --index-url https://download.pytorch.org/whl/cu128
if ($LASTEXITCODE -ne 0) { throw 'CUDA 推理依赖安装失败。' }
$taskLock = Join-Path $PSScriptRoot 'requirements-lock.txt'
if (-not (Test-Path -LiteralPath $taskLock)) { $taskLock = Join-Path $PSScriptRoot 'requirements-dev.txt' }
& $taskPython -m pip install --cache-dir (Join-Path $PSScriptRoot '.cache/pip') -r $taskLock
if ($LASTEXITCODE -ne 0) { throw '项目依赖安装失败。' }
