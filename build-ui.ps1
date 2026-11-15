# 兼容旧构建入口，生成包含真实运行器的完整目录分发。
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1')