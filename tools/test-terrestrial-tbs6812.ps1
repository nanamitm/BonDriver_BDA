# Both TBS6812 slots must be free and already in the requested board modes.
[CmdletBinding()]
param(
    [switch]$RunHardwareTests,
    [ValidateRange(0,1)][int]$TerrestrialSlot=0,
    [ValidateSet('T','S','S3')][string]$PeerSystem='T',
    [string]$TerrestrialChannels='13,14,17'
)
$ErrorActionPreference='Stop'
if (!$RunHardwareTests) { throw 'Use -RunHardwareTests only when both slots are free and their modes are configured.' }
$repo=Split-Path $PSScriptRoot -Parent
$probe=Join-Path $repo 'work\bon-driver-probe-isdbt.exe'
if (!(Test-Path -LiteralPath $probe)) { throw 'Build tools/build-probe.cmd work\bon-driver-probe-isdbt.exe first.' }
$peer=1-$TerrestrialSlot
$prefix="T$TerrestrialSlot-$PeerSystem$peer"
$out=Join-Path $repo 'work\isdb-t-results'
New-Item -ItemType Directory -Force $out | Out-Null
$runs=[Collections.Generic.List[object]]::new()
function Stage-Receiver($system,$slot) {
    $stage=Join-Path $repo "work\isdb-t-matrix\$system$slot"
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item (Join-Path $repo 'x64\Release_Static\BonDriver_BDA.dll') (Join-Path $stage 'BonDriver_Test.dll')
    Copy-Item (Join-Path $repo 'x64\Release_Static\TBS6812.dll') $stage
    $template=switch($system) {
        T { 'config\BonDriver_TBS6812_T.ini' }
        S { 'config\BonDriver_TBS6812_S.ini' }
        S3 { 'config\BonDriver_BDA_4k_auto.ini' }
    }
    $ini=[IO.File]::ReadAllText((Join-Path $repo $template))
    $ini=$ini.Replace('Tuner"',"Tuner $slot"+'"').Replace('Capture"',"Capture $slot"+'"')
    [IO.File]::WriteAllText((Join-Path $stage 'BonDriver_Test.ini'),$ini)
    Join-Path $stage 'BonDriver_Test.dll'
}
function Start-Receiver($dll,$name,$channels,$seconds,$extension) {
    $log=Join-Path $out "$prefix-$name.log"
    $probeArguments=@('"'+$dll+'"',$channels,$seconds,'"'+(Join-Path $out "$prefix-$name.$extension")+'"')
    $process=Start-Process $probe -ArgumentList $probeArguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError (Join-Path $out "$prefix-$name.err")
    $runs.Add($process)
    $null=$process.Handle
    [pscustomobject]@{Process=$process;Log=$log;Name=$name}
}
function Wait-Ready($receiver) {
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        if((Get-Content $receiver.Log -Raw) -match 'first_data_ms='){return}
        if($receiver.Process.HasExited){throw "Receiver exited before data: $($receiver.Name)"}
        Start-Sleep -Milliseconds 100
    } while([DateTime]::UtcNow -lt $deadline)
    throw "Receiver not ready: $($receiver.Name)"
}
function Wait-Receiver($receiver) {
    if(!$receiver.Process.WaitForExit(55000)){throw "Receiver timeout: $($receiver.Name)"}
    $receiver.Process.Refresh()
    $log=Get-Content $receiver.Log -Raw
    if($receiver.Process.ExitCode -ne 0 -or $log -notmatch 'unload_done' -or $log -match 'tune_ok=0|open_ok=0|bytes=0 '){throw "Receiver failed: $($receiver.Name)"}
    Write-Output "PASS $prefix $($receiver.Name)"
}
$holderDll=Stage-Receiver T $TerrestrialSlot
$peerDll=Stage-Receiver $PeerSystem $peer
$firstChannel=($TerrestrialChannels -split ',')[0]
$channels=switch($PeerSystem) {
    T { $TerrestrialChannels }
    S { 'BS01/TS0,ND02/TS0' }
    S3 { 'BS07/TS0,BS14/TS0' }
}
$extension=if($PeerSystem -eq 'S3'){'mmts'}else{'ts'}
try {
    $holder=Start-Receiver $holderDll hold $firstChannel 40 ts
    Wait-Ready $holder
    Wait-Receiver (Start-Receiver $peerDll switch $channels 2 $extension)
    if($holder.Process.HasExited){throw 'Holder exited before peer reopen'}
    Wait-Receiver (Start-Receiver $peerDll reopen (($channels -split ',')[0]) 2 $extension)
    Wait-Receiver $holder
} finally {
    foreach($process in $runs) {
        if(!$process.HasExited){Stop-Process -Id $process.Id -ErrorAction SilentlyContinue}
        $process.Dispose()
    }
}
