[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TVTestDirectory,
    [string]$Subdirectory = '4k8kdrver-built'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$tvtest = (Resolve-Path -LiteralPath $TVTestDirectory).Path
$running = Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -eq (Join-Path $tvtest 'TVTest.exe')
}
if ($running) { throw 'Close this TVTest instance before changing its BonDriver configuration.' }
$destination = [IO.Path]::GetFullPath((Join-Path $tvtest $Subdirectory))
if (!$destination.StartsWith($tvtest.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Deployment directory must be inside the selected TVTest directory.'
}
$artifacts = Join-Path $repo 'x64\Release_Static'
$sources = @{
    'BonDriver_BDA_4k.dll' = Join-Path $artifacts 'BonDriver_BDA.dll'
    'TBS.dll' = Join-Path $artifacts 'TBS.dll'
    'BonDriver_BDA_4k.ini' = Join-Path $repo 'config\BonDriver_BDA_4k.ini'
}
foreach ($source in $sources.Values) {
    if (!(Test-Path -LiteralPath $source)) { throw "Missing build/configuration: $source" }
}
$configPath = Join-Path $tvtest 'BonDriver_dantto4k.ini'
$originalBytes = [IO.File]::ReadAllBytes($configPath)
$encoding = [Text.UTF8Encoding]::new($false, $true)
try { $text = $encoding.GetString($originalBytes) }
catch {
    $encoding = [Text.Encoding]::GetEncoding(932)
    $text = $encoding.GetString($originalBytes)
}
$pattern = '(?m)^[ \t]*bondriverPath[ \t]*=[^\r\n]*'
$settingMatches = [regex]::Matches($text, $pattern)
if ($settingMatches.Count -ne 1) { throw 'Expected exactly one active bondriverPath setting.' }
$relative = '.\' + $Subdirectory + '\BonDriver_BDA_4k.dll'
$updated = [regex]::Replace($text, $pattern, [Text.RegularExpressions.MatchEvaluator]{ param($match) 'bondriverPath=' + $relative })
$backupDirectory = Join-Path $repo 'work\deployment-backups'
New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
$backup = Join-Path $backupDirectory ('BonDriver_dantto4k.ini.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bak')
[IO.File]::WriteAllBytes($backup, $originalBytes)
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$files = foreach ($name in $sources.Keys) {
    $source = $sources[$name]
    $target = Join-Path $destination $name
    Copy-Item -LiteralPath $source -Destination $target
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($sourceHash -ne $targetHash) { throw "Deployment hash mismatch: $name" }
    [pscustomobject]@{ Name = $name; Path = $target; SHA256 = $targetHash }
}
[IO.File]::WriteAllBytes($configPath, $encoding.GetBytes($updated))
if ([IO.File]::ReadAllText($configPath, $encoding) -notmatch [regex]::Escape('bondriverPath=' + $relative)) {
    [IO.File]::WriteAllBytes($configPath, $originalBytes)
    throw 'Configuration verification failed; original bytes were restored.'
}
$manifest = [pscustomobject]@{
    Destination = $destination
    DanttoConfig = $configPath
    OriginalSetting = $settingMatches[0].Value
    UpdatedSetting = 'bondriverPath=' + $relative
    Backup = $backup
    Files = @($files)
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $repo 'work\deployment.json') -Encoding utf8
$manifest
