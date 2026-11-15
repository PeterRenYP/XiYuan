param([switch]$SkipRuntime)
$ErrorActionPreference = 'Stop'
# 默认构建 C++ 原生客户端和独立推理运行器。
& (Join-Path $PSScriptRoot 'build-native.ps1') -SkipRuntime:$SkipRuntime
