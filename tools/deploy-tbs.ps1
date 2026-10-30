[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TVTestDirectory,
    [string]$Subdirectory = '4k8kdrver-built',
    [switch]$Direct,
    [switch]$Dual,
    [switch]$AutoSelect,
    [ValidateSet(0,1)][int]$TunerIndex = 0,
    [string]$DanttoWrapperPath
)
$ErrorActionPreference = 'Stop'
if ($Dual -and !$Direct) { throw '-Dual requires -Direct.' }
if ($PSBoundParameters.ContainsKey('TunerIndex') -and !$Direct) { throw '-TunerIndex requires -Direct.' }
if ($AutoSelect -and (!$Direct -or $Dual -or $PSBoundParameters.ContainsKey('TunerIndex'))) { throw '-AutoSelect requires -Direct and cannot be combined with -Dual or -TunerIndex.' }
if ($Dual -and $TunerIndex -ne 0) { throw '-Dual requires TunerIndex 0 as the primary slot.' }
if ($DanttoWrapperPath) { $DanttoWrapperPath = (Resolve-Path -LiteralPath $DanttoWrapperPath).Path }
$repo = Split-Path $PSScriptRoot -Parent
if ($Direct -and !$PSBoundParameters.ContainsKey('Subdirectory')) { $Subdirectory = '4k8kdrver-direct' }
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
if ($Direct) {
    $sources.Remove('TBS.dll')
    $sources['TBS6812.dll'] = Join-Path $artifacts 'TBS6812.dll'
    $sources['BonDriver_BDA_4k.ini'] = Join-Path $repo 'config\BonDriver_BDA_4k_direct.ini'
    $sources['COPYING-TBS6812.txt'] = Join-Path $repo 'BdaSpecials\TBS6812\COPYING'
    $sources['NOTICE-TBS6812.md'] = Join-Path $repo 'BdaSpecials\TBS6812\NOTICE.md'
    if ($AutoSelect) { $sources['BonDriver_BDA_4k.ini'] = Join-Path $repo 'config\BonDriver_BDA_4k_auto.ini' }
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
if ($AutoSelect) {
    # Every process now reads the same INI; a shared optional dump would collide.
    $updated = [regex]::Replace($updated, '(?m)^[ \t]*mmtsDumpPath[ \t]*=[^\r\n]*', 'mmtsDumpPath=')
}
$backupDirectory = Join-Path $repo 'work\deployment-backups'
New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
$backup = Join-Path $backupDirectory ('BonDriver_dantto4k.ini.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bak')
[IO.File]::WriteAllBytes($backup, $originalBytes)
if ($DanttoWrapperPath) {
    $outer = Join-Path $tvtest 'BonDriver_dantto4k.dll'
    Copy-Item -LiteralPath $outer -Destination (Join-Path $backupDirectory ('BonDriver_dantto4k.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.dll.bak'))
    Copy-Item -LiteralPath $DanttoWrapperPath -Destination $outer
    if ((Get-FileHash $outer).Hash -ne (Get-FileHash $DanttoWrapperPath).Hash) { throw 'Updated dantto4k wrapper hash mismatch.' }
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$files = foreach ($name in $sources.Keys) {
    $source = $sources[$name]
    $target = Join-Path $destination $name
    if ($Direct -and !$AutoSelect -and $TunerIndex -eq 1 -and $name -eq 'BonDriver_BDA_4k.ini') {
        $fixedIni = [IO.File]::ReadAllText($source).Replace('Tuner 0','Tuner 1').Replace('Capture 0','Capture 1')
        [IO.File]::WriteAllText($target, $fixedIni, [Text.UTF8Encoding]::new($false))
        if ([IO.File]::ReadAllText($target) -ne $fixedIni) { throw 'Fixed tuner configuration verification failed.' }
        $sourceHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    } else {
        Copy-Item -LiteralPath $source -Destination $target
        $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    }
    $targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($sourceHash -ne $targetHash) { throw "Deployment hash mismatch: $name" }
    [pscustomobject]@{ Name = $name; Path = $target; SHA256 = $targetHash }
}
[IO.File]::WriteAllBytes($configPath, $encoding.GetBytes($updated))
if ([IO.File]::ReadAllText($configPath, $encoding) -notmatch [regex]::Escape('bondriverPath=' + $relative)) {
    [IO.File]::WriteAllBytes($configPath, $originalBytes)
    throw 'Configuration verification failed; original bytes were restored.'
}
if ($Dual) {
    $secondDirectory = $destination + '-tuner1'
    New-Item -ItemType Directory -Path $secondDirectory -Force | Out-Null
    foreach ($name in $sources.Keys) {
        $target = Join-Path $secondDirectory $name
        if ($name -eq 'BonDriver_BDA_4k.ini') {
            $secondIni = [IO.File]::ReadAllText($sources[$name]).Replace('Tuner 0','Tuner 1').Replace('Capture 0','Capture 1')
            [IO.File]::WriteAllText($target, $secondIni, [Text.UTF8Encoding]::new($false))
            if ([IO.File]::ReadAllText($target) -ne $secondIni) { throw 'Tuner 1 configuration verification failed.' }
        } else {
            Copy-Item -LiteralPath $sources[$name] -Destination $target
            if ((Get-FileHash $target).Hash -ne (Get-FileHash $sources[$name]).Hash) { throw "Tuner 1 hash mismatch: $name" }
        }
    }
    $secondName = 'BonDriver_dantto4k_Tuner1'
    $secondWrapper = Join-Path $tvtest ($secondName + '.dll')
    Copy-Item -LiteralPath (Join-Path $tvtest 'BonDriver_dantto4k.dll') -Destination $secondWrapper
    if ((Get-FileHash $secondWrapper).Hash -ne (Get-FileHash (Join-Path $tvtest 'BonDriver_dantto4k.dll')).Hash) { throw 'Tuner 1 wrapper hash mismatch.' }
    $secondRelative = '.\' + $Subdirectory + '-tuner1\BonDriver_BDA_4k.dll'
    $secondText = [regex]::Replace($updated, $pattern, [Text.RegularExpressions.MatchEvaluator]{ param($match) 'bondriverPath=' + $secondRelative })
    # Separate instances must never write the same optional MMTS dump file.
    $secondText = [regex]::Replace($secondText, '(?m)^[ \t]*mmtsDumpPath[ \t]*=[^\r\n]*', 'mmtsDumpPath=')
    $secondConfig = Join-Path $tvtest ($secondName + '.ini')
    if (Test-Path $secondConfig) {
        Copy-Item -LiteralPath $secondConfig -Destination (Join-Path $backupDirectory ($secondName + '.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.ini.bak'))
    }
    [IO.File]::WriteAllBytes($secondConfig, $encoding.GetBytes($secondText))
    $channels = Join-Path $tvtest 'BonDriver_dantto4k.ch2'
    $secondChannels = Join-Path $tvtest ($secondName + '.ch2')
    if ((Test-Path $channels) -and !(Test-Path $secondChannels)) { Copy-Item -LiteralPath $channels -Destination $secondChannels }
}
if ($AutoSelect) {
    # Retain the former second receiver as a recoverable backup, outside the
    # TVTest BonDriver menu. The second inner directory can remain as fallback.
    foreach ($extension in 'dll','ini','ch2') {
        $previous = Join-Path $tvtest ('BonDriver_dantto4k_Tuner1.' + $extension)
        if (Test-Path -LiteralPath $previous) {
            Move-Item -LiteralPath $previous -Destination (Join-Path $backupDirectory ('BonDriver_dantto4k_Tuner1.' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.' + $extension + '.bak'))
        }
    }
}
$manifest = [pscustomobject]@{
    Destination = $destination
    DanttoConfig = $configPath
    OriginalSetting = $settingMatches[0].Value
    UpdatedSetting = 'bondriverPath=' + $relative
    Backup = $backup
    Direct = [bool]$Direct
    Dual = [bool]$Dual
    AutoSelect = [bool]$AutoSelect
    TunerIndex = $(if ($AutoSelect) { $null } else { $TunerIndex })
    SecondWrapper = $(if ($Dual) { $secondWrapper } else { $null })
    DanttoWrapperSource = $DanttoWrapperPath
    Files = @($files)
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $repo 'work\deployment.json') -Encoding utf8
$manifest
