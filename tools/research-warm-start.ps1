param([string]$ProbePath='work\bon-driver-probe-dual.exe')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$root=Join-Path $repo ('work\warm-validation-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
$stage=Join-Path $root 'raw'
$cache=Join-Path $root 'cache'
New-Item -ItemType Directory -Force $stage,$cache | Out-Null
Copy-Item (Join-Path $repo 'x64\Release_Static\TBS6812.dll') $stage
foreach($mode in 'auto','fixed0','fixed1') {
    Copy-Item (Join-Path $repo 'x64\Release_Static\BonDriver_BDA.dll') (Join-Path $stage ("BonDriver_$mode.dll"))
    $template=if($mode -eq 'auto'){'config\BonDriver_BDA_4k_auto.ini'}else{'config\BonDriver_BDA_4k_direct.ini'}
    $ini=Get-Content (Join-Path $repo $template) -Raw
    if($mode -eq 'fixed1'){$ini=$ini.Replace('Tuner 0','Tuner 1').Replace('Capture 0','Capture 1')}
    $ini=$ini.Replace('LockTimeoutMs=3000',"LockTimeoutMs=3000`nInspectWarmState=1`nExperimentalWarmStart=1`nWarmStartCacheDirectory=$cache").Replace('DebugLog=0','DebugLog=1')
    [IO.File]::WriteAllText((Join-Path $stage ("BonDriver_$mode.ini")),$ini)
}
[IO.File]::WriteAllText((Join-Path $repo 'work\warm-validation-path.txt'),$root)
function Run-Case($mode,$name,$channels,$vendor) {
    $driverLog=Join-Path $stage ("BonDriver_$mode.log")
    $offset=if(Test-Path $driverLog){([IO.File]::ReadAllText($driverLog)).Length}else{0}
    & (Join-Path $repo $ProbePath) (Join-Path $stage ("BonDriver_$mode.dll")) $channels 3 (Join-Path $root ($name+'.mmts')) *> (Join-Path $root ($name+'.log'))
    if($LASTEXITCODE -ne 0){throw "Failed probe: $name exit=$LASTEXITCODE"}
    $diagnostics=([IO.File]::ReadAllText($driverLog)).Substring($offset)
    [IO.File]::WriteAllText((Join-Path $root ($name+'-driver.log')),$diagnostics)
    $usedVendor=$diagnostics -match '\[TuneTiming\] main_tune elapsed'
    if($usedVendor -ne $vendor){throw "Unexpected vendor path: $name actual=$usedVendor expected=$vendor"}
    "PASS $name vendor=$usedVendor"
    Get-Content (Join-Path $root ($name+'.log')) | Select-String 'tune_ms|first_data'
}
Run-Case fixed0 seed0 'BS07/TS0' $true
$state0=@(Get-ChildItem $cache -Filter '*_0.state')
if($state0.Count -ne 1){throw 'Missing normal-close cache for tuner 0'}
$seed=[IO.File]::ReadAllText($state0[0].FullName)
Run-Case fixed1 seed1 'BS07/TS0' $true
foreach($round in 1,2) {
    foreach($slot in 0,1) {Run-Case "fixed$slot" "reuse$slot-round$round" 'BS11/TS0,BS14/TS0,BS07/TS0' $false}
}
[IO.File]::WriteAllText($state0[0].FullName,'corrupt cache')
Run-Case fixed0 corrupt-fallback 'BS11/TS0' $true
# A previously valid record whose stream no longer matches the hardware.
[IO.File]::WriteAllText($state0[0].FullName,$seed)
Run-Case fixed0 stale-fallback 'BS14/TS0' $true
Run-Case fixed0 after-fallback 'BS07/TS0' $false
"RESULTS=$root"
