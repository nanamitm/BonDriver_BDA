# Deploys the all-systems BonDriver into a TVTest or EDCB folder. Run tools/build-tbs.ps1 first.
# Does not write the board mode: the first open of the BonDriver sets both inputs to satellite while the card is free.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    # BonDriver_dantto4k.dll built with passthroughSpaces support.
    [Parameter(Mandatory)][string]$DanttoWrapperPath,
    [string]$Subdirectory = 'tbs6812-all'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$artifacts = Join-Path $repo 'x64\Release_Static'
$target = (Resolve-Path -LiteralPath $Destination).Path
$wrapper = (Resolve-Path -LiteralPath $DanttoWrapperPath).Path
$running = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase) }
if ($running) { throw 'Close the programs running from this folder first.' }
$inner = Join-Path $target $Subdirectory
$outerIni = Join-Path $target 'BonDriver_TBS6812.ini'
$files = @(
    @{ From = $wrapper; To = Join-Path $target 'BonDriver_TBS6812.dll' },
    @{ From = Join-Path $artifacts 'BonDriver_BDA.dll'; To = Join-Path $inner 'BonDriver_TBS6812_Inner.dll' },
    @{ From = Join-Path $artifacts 'TBS6812.dll'; To = Join-Path $inner 'TBS6812.dll' },
    @{ From = Join-Path $repo 'config\BonDriver_TBS6812_Inner.ini'; To = Join-Path $inner 'BonDriver_TBS6812_Inner.ini' },
    @{ From = Join-Path $repo 'BdaSpecials\TBS6812\COPYING'; To = Join-Path $inner 'COPYING-TBS6812.txt' },
    @{ From = Join-Path $repo 'BdaSpecials\TBS6812\NOTICE.md'; To = Join-Path $inner 'NOTICE-TBS6812.md' }
)
foreach ($file in $files) {
    if (!(Test-Path -LiteralPath $file.From)) { throw "Missing: $($file.From)" }
}
$backup = Join-Path $repo ('work\deploy-backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $inner, $backup | Out-Null
foreach ($file in $files) {
    if (Test-Path -LiteralPath $file.To) { Copy-Item -LiteralPath $file.To -Destination $backup }
    Copy-Item -LiteralPath $file.From -Destination $file.To
    if ((Get-FileHash -LiteralPath $file.From).Hash -ne (Get-FileHash -LiteralPath $file.To).Hash) { throw "Copy mismatch: $($file.To)" }
}
# The card reader name in an existing INI belongs to this machine.
if (Test-Path -LiteralPath $outerIni) {
    Write-Output "Kept the existing $outerIni"
} else {
    Copy-Item -LiteralPath (Join-Path $repo 'config\BonDriver_TBS6812.ini') -Destination $outerIni
    Write-Output "Created ${outerIni}: set smartCardReaderName for ISDB-S3"
}
Write-Output "Deployed to $target; backup in $backup"
