# Packages the Release_Static x64 build. Run tools/build-tbs.ps1 first.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$OutputDirectory = '.'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$artifacts = Join-Path $repo 'x64\Release_Static'
$package = "BonDriver_BDA-TBS6812-$Version-win64"
$stage = Join-Path ([IO.Path]::GetTempPath()) "$package-$PID"
if (!(Test-Path -LiteralPath $OutputDirectory)) { New-Item -ItemType Directory -Path $OutputDirectory | Out-Null }
$zip = Join-Path (Resolve-Path -LiteralPath $OutputDirectory).Path "$package.zip"
try {
    foreach ($directory in 'config', 'doc', 'tools') { New-Item -ItemType Directory -Force (Join-Path $stage $directory) | Out-Null }
    # A missing file fails the package instead of shipping an incomplete release.
    Copy-Item -LiteralPath (Join-Path $artifacts 'BonDriver_BDA.dll'), (Join-Path $artifacts 'TBS6812.dll') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repo 'work\tbs6812-board-mode.exe') -Destination (Join-Path $stage 'tools')
    Copy-Item -Path (Join-Path $repo 'config\BonDriver_TBS6812*.ini') -Destination (Join-Path $stage 'config')
    foreach ($document in 'tbs6812-all', 'tbs6812-direct', 'tbs6812-isdb-s', 'tbs6812-isdb-t', 'tbs6812-j83b') {
        Copy-Item -LiteralPath (Join-Path $repo "doc\$document.md") -Destination (Join-Path $stage 'doc')
    }
    Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repo 'BdaSpecials\TBS6812\COPYING') -Destination (Join-Path $stage 'COPYING-TBS6812.txt')
    Copy-Item -LiteralPath (Join-Path $repo 'BdaSpecials\TBS6812\NOTICE.md') -Destination (Join-Path $stage 'NOTICE-TBS6812.md')
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
$zip
