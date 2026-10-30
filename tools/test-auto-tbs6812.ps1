[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DriverPath,
    [Parameter(Mandatory)][string]$Fixed0Path,
    [Parameter(Mandatory)][string]$Fixed1Path,
    [Parameter(Mandatory)][string]$SpecialLogPath,
    [string]$OutputDirectory = 'work\auto-test',
    [string]$ProbePath = 'work\bon-driver-probe-dual.exe',
    [switch]$Dantto
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$probe = Join-Path $repo $ProbePath
$runs = [Collections.Generic.List[object]]::new()
function Start-Probe($driver, $name, $channel, $seconds) {
    $log = Join-Path $output ($name+'.log')
    $extension = if ($Dantto) { '.ts' } else { '.mmts' }
    $capture = Join-Path $output ($name+$extension)
    $process = Start-Process -FilePath $probe -ArgumentList @('"'+$driver+'"',$channel,$seconds,'"'+$capture+'"') -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError (Join-Path $output ($name+'.err'))
    $null = $process.Handle
    $run = [pscustomobject]@{Process=$process; Log=$log; Name=$name; Slot=-1}
    $runs.Add($run)
    return $run
}
function Wait-Ready($run, $slot) {
    $deadline = [DateTime]::UtcNow.AddSeconds(35)
    do {
        if ((Test-Path $run.Log) -and (Get-Content $run.Log -Raw) -match 'first_data_ms=') {
            $selection=[regex]::Match((Get-Content $SpecialLogPath -Raw),"pid=$($run.Process.Id) tuner=([01]) initialized:")
            if (!$selection.Success) { throw "No selected tuner in log: $($run.Name)" }
            $run.Slot=[int]$selection.Groups[1].Value
            if ($slot -ge 0 -and $run.Slot -ne $slot) { throw "Wrong selected tuner: $($run.Name)" }
            Write-Output "PASS $($run.Name) selected tuner=$($run.Slot) pid=$($run.Process.Id)"
            return
        }
        if ($run.Process.HasExited) { throw "Probe exited before data: $($run.Name)" }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "No data within 35 seconds: $($run.Name)"
}
function Wait-Probe($run, $expectedExit=0) {
    if (!$run.Process.WaitForExit(90000)) { throw "Probe timeout: $($run.Name)" }
    $run.Process.Refresh()
    $text = Get-Content $run.Log -Raw
    if ($run.Process.ExitCode -ne $expectedExit) { throw "Unexpected exit $($run.Process.ExitCode): $($run.Name)" }
    if ($expectedExit -eq 0 -and ($text -notmatch 'unload_done' -or $text -match 'tune_ok=0|bytes=0 ')) { throw "Capture failed: $($run.Name)" }
    if ($expectedExit -eq 6 -and $text -notmatch 'open_ok=0') { throw "Busy tuner was not rejected at OpenTuner: $($run.Name)" }
    Write-Output "PASS $($run.Name) exit=$expectedExit"
}
try {
    $first = Start-Probe $DriverPath 'auto-first' 'BS07/TS0' 20
    Wait-Ready $first -1
    # Enumeration order is supplied by Windows. Explicitly pinning the busy
    # first slot must fail while the other slot is still free.
    $firstFixed = if ($first.Slot -eq 0) { $Fixed0Path } else { $Fixed1Path }
    $secondFixed = if ($first.Slot -eq 0) { $Fixed1Path } else { $Fixed0Path }
    Wait-Probe (Start-Probe $firstFixed 'fixed-first-busy' 'BS07/TS0' 1) 6
    $second = Start-Probe $DriverPath 'auto-second' 'BS11/TS0' 45
    Wait-Ready $second (1-$first.Slot)
    Wait-Probe (Start-Probe $DriverPath 'auto-all-busy' 'BS07/TS0' 1) 6
    Wait-Probe $first
    # The first slot is now free; pinning the busy second slot must still fail.
    Wait-Probe (Start-Probe $secondFixed 'fixed-second-busy' 'BS07/TS0' 1) 6
    $reopen = Start-Probe $DriverPath 'auto-reopen' 'BS14/TS0' 4
    Wait-Ready $reopen $first.Slot
    Wait-Probe $reopen
    Wait-Probe $second
    # Verify both fixed configurations can also open when free.
    foreach ($slot in 0,1) {
        $driver = if ($slot -eq 0) { $Fixed0Path } else { $Fixed1Path }
        $fixed = Start-Probe $driver "fixed$slot-free" 'BS07/TS0' 3
        Wait-Ready $fixed $slot
        Wait-Probe $fixed
    }
} finally {
    foreach ($run in $runs) {
        if (!$run.Process.HasExited) { $run.Process.Kill(); $null=$run.Process.WaitForExit(5000) }
        $run.Process.Dispose()
    }
}
