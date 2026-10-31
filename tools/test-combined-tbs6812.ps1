# Slot 0 must be free and already in terrestrial mode, with terrestrial + optical signals.
[CmdletBinding()]
param([switch]$RunHardwareTests)
$ErrorActionPreference='Stop'
if(!$RunHardwareTests){throw 'Use -RunHardwareTests only when TBS6812 slot 0 is free and terrestrial mode/signals are configured.'}
$repo=Split-Path $PSScriptRoot -Parent
$probe=Join-Path $repo 'work\bon-driver-probe-isdbt.exe'
$stage=Join-Path $repo 'work\combined-stage'
$out=Join-Path $repo 'work\combined-results'
New-Item -ItemType Directory -Force $stage,$out | Out-Null
$dll=Join-Path $stage 'BonDriver_BDA.T.dll'
Copy-Item (Join-Path $repo 'x64\Release_Static\BonDriver_BDA.dll') $dll
Copy-Item (Join-Path $repo 'x64\Release_Static\TBS6812.dll') $stage
$ini=[IO.File]::ReadAllText((Join-Path $repo 'config\BonDriver_TBS6812_T.ini')).Replace('Tuner"','Tuner 0"').Replace('Capture"','Capture 0"')
[IO.File]::WriteAllText((Join-Path $stage 'BonDriver_BDA.T.ini'),$ini)
# Channel names below are receivable in the validation environment (Okinawa).
foreach($case in @(
    @{Name='terrestrial-first';Channels='13,H004,14,H005,17,H030,H031,13'},
    @{Name='cable-first';Channels='H004,13,H005,14,H030,17'},
    @{Name='offair-recovery';Channels='51,H004,13';ExpectNoLock=$true}
)) {
    $probeArguments=@($dll,$case.Channels,'3',(Join-Path $out ($case.Name+'.ts')))
    if($case.ExpectNoLock){$probeArguments+='--expect-initial-no-lock'}
    & $probe @probeArguments | Tee-Object -FilePath (Join-Path $out ($case.Name+'.log'))
    if($LASTEXITCODE -ne 0){throw "Combined receiver failed: $($case.Name)"}
    Write-Output "PASS $($case.Name)"
}
