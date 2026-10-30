[CmdletBinding()]
param([string]$StageDirectory = 'work\dual-stage', [string]$OutputDirectory = 'work\dual-test', [switch]$Dantto,
    [string]$ProbePath = 'work\bon-driver-probe.exe')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$stage = (Resolve-Path (Join-Path $repo $StageDirectory)).Path
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$probe = Join-Path $repo $ProbePath
function Start-Probe($slot, $name, $channels, $seconds) {
    $log = Join-Path $output ($name + '.log')
    $driverName = if ($Dantto) { 'BonDriver_dantto4k.dll' } else { 'BonDriver_BDA_4k.dll' }
    $extension = if ($Dantto) { '.ts' } else { '.mmts' }
    $dll = Join-Path $stage "tuner$slot\$driverName"
    $capture = Join-Path $output ($name + $extension)
    $process = Start-Process -FilePath $probe -ArgumentList @('"' + $dll + '"', $channels, $seconds, '"' + $capture + '"') -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError (Join-Path $output ($name + '.err'))
    $null = $process.Handle
    [pscustomobject]@{ Process=$process; Log=$log; Name=$name }
}
function Wait-Ready($probeRun) {
    $deadline = [DateTime]::UtcNow.AddSeconds(35)
    do {
        if ((Test-Path $probeRun.Log) -and (Get-Content $probeRun.Log -Raw) -match 'first_data_ms=') { return }
        if ($probeRun.Process.HasExited) { throw "Probe exited before receiving: $($probeRun.Name)" }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "No data within 35 seconds: $($probeRun.Name)"
}
function Wait-Probe($probeRun) {
    if (!$probeRun.Process.WaitForExit(90000)) { throw "Probe timeout: $($probeRun.Name)" }
    $probeRun.Process.Refresh()
    $text = Get-Content $probeRun.Log -Raw
    if ($probeRun.Process.ExitCode -ne 0 -or $text -notmatch 'unload_done' -or $text -match 'tune_ok=0|open_ok=0|bytes=0 ') { throw "Failed probe: $($probeRun.Name)" }
    Write-Output "PASS $($probeRun.Name) exit=$($probeRun.Process.ExitCode)"
}
# Keep one carrier receiving while the other slot bootstraps, changes RF/TSID,
# closes, and opens again. Repeat with the roles reversed.
foreach ($holder in 0,1) {
    $peer = 1 - $holder
    $steady = Start-Probe $holder "hold$holder" 'BS07/TS0' 55
    Wait-Ready $steady
    $switching = Start-Probe $peer "switch$peer" 'BS11/TS0,BS14/TS0,BS07/TS1,BS07/TS0' 3
    Wait-Probe $switching
    $reopen = Start-Probe $peer "reopen$peer" 'BS11/TS0' 4
    Wait-Probe $reopen
    Wait-Probe $steady
}
