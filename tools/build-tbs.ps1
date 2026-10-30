[CmdletBinding()]
param(
    [string]$VCToolsVersion = '14.44.35207',
    [string]$PlatformToolset = 'v143'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $locator -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (!$installation) { throw 'Visual Studio MSBuild was not found.' }
$atl = Join-Path $installation "VC\Tools\MSVC\$VCToolsVersion\atlmfc\include\atlbase.h"
if (!(Test-Path -LiteralPath $atl)) { throw "ATL headers were not found: $atl" }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
$logs = Join-Path $repo 'work\build-logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$projects = @(
    'projects\BaseClasses\BaseClasses.vcxproj',
    'projects\muparser\muparser.vcxproj',
    'BdaSpecials\TBS\TBS.vcxproj',
    'BonDriver_BDA\BonDriver.vcxproj'
)
foreach ($project in $projects) {
    $log = Join-Path $logs ((Split-Path $project -Leaf) + '.log')
    & $msbuild (Join-Path $repo $project) /nologo /m /v:minimal `
        /p:Configuration=Release_Static /p:Platform=x64 `
        "/p:PlatformToolset=$PlatformToolset" "/p:VCToolsVersion=$VCToolsVersion" `
        "/p:SolutionDir=$repo\" /fl "/flp:logfile=$log;verbosity=normal"
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $project (see $log)" }
}
$artifacts = Join-Path $repo 'x64\Release_Static'
Get-Item -LiteralPath (Join-Path $artifacts 'BonDriver_BDA.dll'), (Join-Path $artifacts 'TBS.dll')
