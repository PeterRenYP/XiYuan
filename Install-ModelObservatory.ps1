param([string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs/ModelObservatory'))
$ErrorActionPreference = 'Stop'
# 按用户安装，无需管理员；只创建本产品快捷方式，不修改系统依赖。
$taskDestination = [IO.Path]::GetFullPath($Destination)
$taskSource = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
if ($taskDestination -eq $taskSource -or $taskDestination.StartsWith($taskSource + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '安装位置不能位于分发目录内。' }
if (-not (Test-Path -LiteralPath (Join-Path $taskSource 'ModelObservatory.exe'))) { throw '请在完整分发目录内运行安装脚本。' }
New-Item -ItemType Directory -Path $taskDestination -Force | Out-Null
Get-ChildItem -LiteralPath $PSScriptRoot | Copy-Item -Destination $taskDestination -Recurse -Force
$taskShell = New-Object -ComObject WScript.Shell
$taskShortcut = $taskShell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) '模型观测台.lnk'))
$taskShortcut.TargetPath = Join-Path $taskDestination 'ModelObservatory.exe'
$taskShortcut.WorkingDirectory = $taskDestination
$taskShortcut.Save()
Write-Host "安装完成：$taskDestination"
