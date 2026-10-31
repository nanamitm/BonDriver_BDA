# Two receivers switch across all delivery systems at once. Both inputs must be free and in satellite mode.
# Channel names are receivable in the validation environment (Okinawa); pass your own for elsewhere.
# Build the probe with tools\build-probe.cmd. Results stay under work\all-results.
[CmdletBinding()]
param(
    [switch]$RunHardwareTests,
    [Parameter(Mandatory)][string]$BonDriver,
    [string]$First = '13ch,BS07/TS0,H004,BS01/TS0,14ch',
    [string]$Second = 'BS11/TS0,17ch,H005,ND02/TS0,BS14/TS0,17ch',
    [int]$Seconds = 4,
    # dantto4k.exe, to check each capture with --verify.
    [string]$Verifier
)
$ErrorActionPreference = 'Stop'
if (!$RunHardwareTests) { throw 'Use -RunHardwareTests only when both inputs are free and in satellite mode.' }
$repo = Split-Path $PSScriptRoot -Parent
$driver = (Resolve-Path -LiteralPath $BonDriver).Path
$probe = Join-Path $repo 'work\bon-driver-probe.exe'
$out = Join-Path $repo 'work\all-results'
New-Item -ItemType Directory -Force $out | Out-Null
$started = Get-Date
$start = {
    param($Name, $Channels)
    Start-Process -FilePath $probe -ArgumentList "`"$driver`"", "`"$Channels`"", $Seconds, "`"$(Join-Path $out "$Name.ts")`"" `
        -WorkingDirectory (Split-Path $driver) -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $out "$Name.log")
}
$firstReceiver = & $start 'first' $First
$null = $firstReceiver.Handle # ExitCode stays empty unless the handle was read.
Start-Sleep -Seconds 3
$secondReceiver = & $start 'second' $Second
$null = $secondReceiver.Handle
foreach ($process in $firstReceiver, $secondReceiver) {
    if (!$process.WaitForExit(($Seconds + 30) * 8 * 1000)) { $process.Kill(); throw 'A receiver did not finish in time.' }
    if ($process.ExitCode -ne 0) { throw "A receiver failed; see $out" }
}
$failed = 0
foreach ($capture in Get-ChildItem -LiteralPath $out -Filter '*-*.ts' | Where-Object { $_.Name -match '^(first|second)-' -and $_.LastWriteTime -ge $started }) {
    $bytes = [IO.File]::ReadAllBytes($capture.FullName)
    # From the first synchronized packet: the start of a capture is what direct switching must keep clean.
    $offset = 0
    while ($offset + 188 -lt $bytes.Length -and !($bytes[$offset] -eq 0x47 -and $bytes[$offset + 188] -eq 0x47)) { $offset++ }
    $sync = $tei = $continuity = 0
    $last = @{}
    for ($i = $offset; $i + 188 -le $bytes.Length; $i += 188) {
        if ($bytes[$i] -ne 0x47) { $sync++; continue }
        if ($bytes[$i + 1] -band 0x80) { $tei++ }
        $pid_ = (($bytes[$i + 1] -band 0x1F) -shl 8) -bor $bytes[$i + 2]
        if ($pid_ -ne 0x1FFF -and ($bytes[$i + 3] -band 0x10)) {
            $counter = $bytes[$i + 3] -band 0x0F
            if ($last.ContainsKey($pid_) -and $counter -ne $last[$pid_] -and $counter -ne (($last[$pid_] + 1) -band 0x0F)) { $continuity++ }
            $last[$pid_] = $counter
        }
    }
    Write-Output "$($capture.Name): sync $sync, TEI $tei, continuity $continuity"
    if ($sync -or $tei -or $continuity) { $failed++ }
    if ($Verifier) {
        & $Verifier --verify $capture.FullName | Select-Object -Last 1
    }
}
if ($failed) { throw "$failed capture(s) have errors; see $out" }
Write-Output 'PASS: all captures are clean from their first packet'
