# Input 0 must already be terrestrial mode with UHF13 + optical service;
# input 1 must already be satellite mode. No board-mode writes are made here.
[CmdletBinding()]
param([switch]$RunHardwareTests,[string]$BootstrapChannel='13',[ValidateRange(90000,770000)][int]$BootstrapFrequencyKHz=473143,[string]$Channels='H004,H005,H030,H031',[string]$OffAirChannel='')
$ErrorActionPreference='Stop'
if(!$RunHardwareTests){throw 'Use -RunHardwareTests only when both tuners are free and modes/signals are configured.'}
$repo=Split-Path $PSScriptRoot -Parent
$probe=Join-Path $repo 'work\bon-driver-probe-isdbt.exe'
$out=Join-Path $repo 'work\j83b-results'
New-Item -ItemType Directory -Force $out | Out-Null
$runs=[Collections.Generic.List[object]]::new()
function Stage($template,$name,$slot) {
    $directory=Join-Path $repo "work\j83b-matrix\$name"
    New-Item -ItemType Directory -Force $directory | Out-Null
    Copy-Item (Join-Path $repo 'x64\Release_Static\BonDriver_BDA.dll') (Join-Path $directory 'BonDriver_Test.dll')
    Copy-Item (Join-Path $repo 'x64\Release_Static\TBS6812.dll') $directory
    $ini=[IO.File]::ReadAllText((Join-Path $repo $template))
    $ini=$ini.Replace('BootstrapFrequencyKHz=473143',"BootstrapFrequencyKHz=$BootstrapFrequencyKHz")
    $ini=$ini.Replace('Tuner"',"Tuner $slot"+'"').Replace('Capture"',"Capture $slot"+'"')
    [IO.File]::WriteAllText((Join-Path $directory 'BonDriver_Test.ini'),$ini)
    Join-Path $directory 'BonDriver_Test.dll'
}
function Start-Receiver($dll,$name,$channels,$seconds,$extension,[switch]$ExpectInitialNoLock) {
    $log=Join-Path $out "$name.log"
    $probeArguments=@('"'+$dll+'"',$channels,$seconds,'"'+(Join-Path $out "$name.$extension")+'"')
    if($ExpectInitialNoLock){$probeArguments+='--expect-initial-no-lock'}
    $process=Start-Process $probe -ArgumentList $probeArguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError (Join-Path $out "$name.err")
    $runs.Add($process)
    $null=$process.Handle
    [pscustomobject]@{Process=$process;Log=$log;Name=$name}
}
function Ready($receiver) {
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        if((Get-Content $receiver.Log -Raw) -match 'first_data_ms='){return}
        if($receiver.Process.HasExited){throw "Receiver exited before data: $($receiver.Name)"}
        Start-Sleep -Milliseconds 100
    } while([DateTime]::UtcNow -lt $deadline)
    throw "Receiver not ready: $($receiver.Name)"
}
function Done($receiver) {
    if(!$receiver.Process.WaitForExit(55000)){throw "Receiver timeout: $($receiver.Name)"}
    $receiver.Process.Refresh()
    if($receiver.Process.ExitCode -ne 0 -or (Get-Content $receiver.Log -Raw) -notmatch 'unload_done'){throw "Receiver failed: $($receiver.Name)"}
    Write-Output "PASS $($receiver.Name)"
}
$satellite=Stage 'config\BonDriver_BDA_4k_auto.ini' s3 1
$cable=Stage 'config\BonDriver_TBS6812_J83B.ini' j83b 0
$terrestrial=Stage 'config\BonDriver_TBS6812_T.ini' terrestrial 0
$first=($Channels -split ',')[0]
try {
    $holder=Start-Receiver $satellite mixed-s3-hold 'BS07/TS0' 65 mmts
    Ready $holder
    if($OffAirChannel){Done (Start-Receiver $cable no-lock-recovery "$OffAirChannel,$first" 2 ts -ExpectInitialNoLock)}
    Done (Start-Receiver $cable mixed-j83b-switch $Channels 2 ts)
    Done (Start-Receiver $cable mixed-j83b-reopen $first 2 ts)
    if($holder.Process.HasExited){throw 'Satellite holder exited before terrestrial handback test'}
    Done (Start-Receiver $terrestrial mixed-terrestrial-return $BootstrapChannel 3 ts)
    Done $holder
} finally {
    foreach($process in $runs) {
        if(!$process.HasExited){Stop-Process -Id $process.Id -ErrorAction SilentlyContinue}
        $process.Dispose()
    }
}
