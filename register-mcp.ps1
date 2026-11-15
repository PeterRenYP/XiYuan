param([string]$CodexPath)
$ErrorActionPreference = 'Stop'
# 只注册本项目的新服务，已有 MCP 配置由 Codex 官方命令保留。
$taskPython = Join-Path $PSScriptRoot '.venv-mcp/Scripts/python.exe'
$taskEntry = Join-Path $PSScriptRoot 'run_mcp.py'
if (-not (Test-Path -LiteralPath $taskPython)) { throw '请先运行 setup-mcp.ps1。' }
if (-not $CodexPath) {
    $taskCommand = Get-Command codex -ErrorAction SilentlyContinue
    if ($taskCommand) { $CodexPath = $taskCommand.Source }
    else {
        $taskBin = Join-Path $env:LOCALAPPDATA 'OpenAI/Codex/bin'
        $taskExecutable = Get-ChildItem -LiteralPath $taskBin -Filter codex.exe -Recurse -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($taskExecutable) { $CodexPath = $taskExecutable.FullName }
    }
}
if (-not $CodexPath -or -not (Test-Path -LiteralPath $CodexPath)) { throw '未找到 Codex，请用 -CodexPath 指定 codex.exe。' }
& $CodexPath mcp add model_observatory -- $taskPython -X utf8 $taskEntry
if ($LASTEXITCODE -ne 0) { throw 'MCP 注册失败。' }
# 事件等待最多六十秒，工具超时留出连接与协议开销；只修改新服务所属配置段。
$taskConfigRoot = $env:CODEX_HOME
if (-not $taskConfigRoot) { $taskConfigRoot = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.codex' }
$taskConfig = Join-Path $taskConfigRoot 'config.toml'
$taskContent = [IO.File]::ReadAllText($taskConfig)
$taskBlock = [regex]::Match($taskContent, '(?ms)^\[mcp_servers\.model_observatory\]\r?\n.*?(?=^\[|\z)')
if (-not $taskBlock.Success) { throw '服务已注册，但未找到对应配置段，请参考 mcp-codex.toml 设置工具超时。' }
$taskUpdated = [regex]::Replace($taskBlock.Value, '(?m)^(startup_timeout_sec|tool_timeout_sec)\s*=.*(?:\r?\n|$)', '')
$taskUpdated = $taskUpdated.TrimEnd() + "`nstartup_timeout_sec = 15`ntool_timeout_sec = 90`n`n"
$taskResult = $taskContent.Remove($taskBlock.Index, $taskBlock.Length).Insert($taskBlock.Index, $taskUpdated)
$taskTemporary = $taskConfig + '.model-observatory.tmp'
[IO.File]::WriteAllText($taskTemporary, $taskResult, [Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $taskTemporary -Destination $taskConfig -Force
Write-Host '已注册 model_observatory。若当前会话未出现工具，请重启 Codex 后使用。'
