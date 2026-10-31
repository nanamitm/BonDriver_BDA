# Opt-in hardware reception test. Both TBS6812 slots must be free.
[CmdletBinding()]
param([switch]$RunHardwareTests)
$ErrorActionPreference='Stop'
if (!$RunHardwareTests) { throw 'Use -RunHardwareTests only when both satellite tuners are free.' }
$repo=Split-Path $PSScriptRoot -Parent
$cache=Join-Path $repo 'work\satellite-matrix-cache'
foreach ($slot in 0,1) {
 foreach ($mode in 's','s3') {
  $directory=Join-Path $repo "work\satellite-matrix\$mode$slot"
  New-Item -ItemType Directory -Force $directory | Out-Null
  Copy-Item (Join-Path $repo 'x64\Release_Static\BonDriver_BDA.dll') (Join-Path $directory 'BonDriver_Test.dll')
  Copy-Item (Join-Path $repo 'x64\Release_Static\TBS6812.dll') $directory
  $template=if($mode -eq 's'){'config\BonDriver_TBS6812_S.ini'}else{'config\BonDriver_BDA_4k_auto.ini'}
  $ini=[IO.File]::ReadAllText((Join-Path $repo $template))
  $ini=$ini.Replace('Tuner"',"Tuner $slot"+'"').Replace('Capture"',"Capture $slot"+'"')
  $ini=$ini -replace '(?m)^(ExperimentalWarmStart|WarmStartCacheDirectory)=.*\r?\n',''
  $ini=$ini.Replace('[TBS6812]',"[TBS6812]`r`nExperimentalWarmStart=1`r`nWarmStartCacheDirectory=$cache")
  [IO.File]::WriteAllText((Join-Path $directory 'BonDriver_Test.ini'),$ini)
 }
}
$runs=[Collections.Generic.List[object]]::new()
$probe=Join-Path $repo 'work\bon-driver-probe-dual.exe'
$out=Join-Path $repo 'work\isdb-s-results'
New-Item -ItemType Directory -Force $out | Out-Null
function Start-Receiver($mode,$slot,$name,$channels,$seconds) {
 $dll=Join-Path $repo "work\satellite-matrix\$mode$slot\BonDriver_Test.dll"
 $ext=if($mode -eq 's'){'ts'}else{'mmts'}
 $args=@('"'+$dll+'"',$channels,$seconds,'"'+(Join-Path $out "$name.$ext")+'"')
 $p=Start-Process $probe -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out "$name.log") -RedirectStandardError (Join-Path $out "$name.err")
 $runs.Add($p)
 $null=$p.Handle
 [pscustomobject]@{Process=$p;Name=$name;Log=(Join-Path $out "$name.log")}
}
function Ready($r) {
 $deadline=[DateTime]::UtcNow.AddSeconds(30)
 while([DateTime]::UtcNow -lt $deadline) {
  if((Get-Content $r.Log -Raw) -match 'first_data_ms='){return}
  if($r.Process.HasExited){throw "Receiver exited: $($r.Name)"}
  Start-Sleep -Milliseconds 100
 }
 throw "Receiver not ready: $($r.Name)"
}
function Done($r) {
 if(!$r.Process.WaitForExit(55000)){throw "Receiver timeout: $($r.Name)"}
 $r.Process.Refresh()
 if($r.Process.ExitCode -ne 0){throw "Receiver failed: $($r.Name)"}
 Write-Output "PASS $($r.Name)"
}
try {
$h=Start-Receiver 's' 0 'hold-s0' 'BS01/TS0' 40
Ready $h
Done (Start-Receiver 's' 1 'ss-peer' 'BS01/TS1,ND02/TS0' 2)
Done (Start-Receiver 's3' 1 'mixed-s3-peer' 'BS07/TS0,BS11/TS0,BS14/TS0' 2)
Done (Start-Receiver 's' 1 'handback-s-peer' 'BS01/TS0,ND04/TS0' 2)
Done $h
$h=Start-Receiver 's3' 1 'hold-s3-1' 'BS07/TS0' 25
Ready $h
Done (Start-Receiver 's' 0 'mixed-s-peer' 'BS03/TS0,BS01/TS1,ND02/TS0' 2)
Done $h

} finally {
 foreach ($process in $runs) {
  if (!$process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
  $process.Dispose()
 }
}
