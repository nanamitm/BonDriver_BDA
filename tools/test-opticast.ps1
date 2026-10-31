# Exercises the real DLL's INI parser without OpenTuner or hardware access.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$probe=Join-Path $repo 'work\bon-driver-list.exe'
$template=[IO.File]::ReadAllText((Join-Path $repo 'tests\fixtures\BonDriver_TBS6812_J83B.ini')).Replace('DebugLog=0','DebugLog=1')
$reference=@(Get-Content (Join-Path $repo 'tests\fixtures\opticast-frequencies.tsv') | Where-Object {$_ -notmatch '^#' -and $_.Trim()} | ForEach-Object {
    $fields=$_ -split "`t"
    [pscustomobject]@{Physical=$fields[1];Frequency=[int]$fields[2]}
})
if($reference.Count -ne 109){throw 'Reference must contain 109 channels'}
$base=@($reference | Where-Object {($_.Frequency-93250)%6500 -eq 0})
$cases=@(
    @{Name='all';Options='All';Expected=$reference},
    @{Name='default';Options='';Expected=$base},
    @{Name='h11';Options='H011+';Expected=@($base | ForEach-Object {
        [pscustomobject]@{Physical=$_.Physical;Frequency=$_.Frequency+$(if($_.Physical -in 'H011','H012'){1000}else{0})}
    })},
    @{Name='h26';Options='H026+';Expected=@($base | ForEach-Object {
        [pscustomobject]@{Physical=$_.Physical;Frequency=$_.Frequency+$(if($_.Physical -in 'H026','H027','H028'){4000}else{0})}
    })},
    @{Name='combined';Options='H011+,H026+';Expected=@($base | ForEach-Object {
        $shift=if($_.Physical -in 'H011','H012'){1000}elseif($_.Physical -in 'H026','H027','H028'){4000}else{0}
        [pscustomobject]@{Physical=$_.Physical;Frequency=$_.Frequency+$shift}
    })},
    @{Name='explicit-h44';Generate='OPTICAST-44+';Expected=@([pscustomobject]@{Physical='H044';Frequency=374950})},
    @{Name='explicit-h105';Generate='OPTICAST:104:1';Expected=@([pscustomobject]@{Physical='H105';Frequency=769250})}
)
foreach($case in $cases) {
    $stage=Join-Path $repo "work\opticast-test\$($case.Name)"
    New-Item -ItemType Directory -Force $stage | Out-Null
    $dll=Join-Path $stage 'BonDriver_Test.dll'
    Copy-Item (Join-Path $repo 'x64\Release_Static\BonDriver_BDA.dll') $dll
    $ini=$template.Replace('ChannelSettingsAutoOptions="All"','ChannelSettingsAutoOptions="'+$case.Options+'"')
    if($case.Generate){$ini=$ini.Replace('ChannelSettingsAuto="Opticast"','ChannelGenerate0="'+$case.Generate+',0,,,0"')}
    [IO.File]::WriteAllText((Join-Path $stage 'BonDriver_Test.ini'),$ini)
    $logPath=Join-Path $stage 'BonDriver_Test.log'
    if(Test-Path -LiteralPath $logPath){Remove-Item -LiteralPath $logPath}
    $listed=@(& $probe $dll)
    if($LASTEXITCODE -ne 0){throw "DLL enumeration failed: $($case.Name)"}
    $log=[IO.File]::ReadAllText($logPath)
    if($log -match 'OpenTuner|InitDSFilterEnum'){throw 'Configuration test unexpectedly opened a tuner'}
    $rows=[regex]::Matches($log,'(?m)^TUNINGSPACE00: \(auto\) CH(\d+)=\d+,(\d+)\.(\d+),[^,]*,\d+,([^,\r\n]+),')
    $expected=@($case.Expected)
    if($rows.Count -ne $expected.Count -or $listed.Count -ne $expected.Count){throw "Channel count mismatch: $($case.Name)"}
    for($i=0;$i -lt $rows.Count;$i++) {
        $row=$rows[$i]
        $frequency=[int]$row.Groups[2].Value*1000+[int]$row.Groups[3].Value
        $physical=[regex]::Match($row.Groups[4].Value,'^H\d{3}').Value
        $enumerated=$listed[$i] -split "`t"
        if([int]$row.Groups[1].Value -ne $i -or $frequency -ne $expected[$i].Frequency -or $physical -ne $expected[$i].Physical -or
            [int]$enumerated[1] -ne $i -or $enumerated[2] -ne $row.Groups[4].Value){throw "Channel mapping mismatch: $($case.Name) index=$i"}
    }
    Write-Output "PASS $($case.Name): $($rows.Count) channels, indices/names/frequencies match"
}

# Verify that the combined template still exposes both spaces through the real DLL.
$stage=Join-Path $repo 'work\opticast-test\terrestrial-combined'
New-Item -ItemType Directory -Force $stage | Out-Null
$dll=Join-Path $stage 'BonDriver_Test.dll'
Copy-Item (Join-Path $repo 'x64\Release_Static\BonDriver_BDA.dll') $dll
$ini=[IO.File]::ReadAllText((Join-Path $repo 'config\BonDriver_TBS6812_T.ini')).Replace('DebugLog=0','DebugLog=1')
[IO.File]::WriteAllText((Join-Path $stage 'BonDriver_Test.ini'),$ini)
$logPath=Join-Path $stage 'BonDriver_Test.log'
if(Test-Path -LiteralPath $logPath){Remove-Item -LiteralPath $logPath}
$listed=@(& $probe $dll)
if($LASTEXITCODE -ne 0){throw 'Combined enumeration failed'}
$uhf=@($listed | Where-Object {$_ -match '^0\t'})
$optical=@($listed | Where-Object {$_ -match '^1\t'})
if($uhf.Count -ne 40 -or $optical.Count -ne 109){throw 'Combined template must have 40 UHF (13..52) and 109 optical channels'}
$log=[IO.File]::ReadAllText($logPath)
$rows=[regex]::Matches($log,'(?m)^TUNINGSPACE01: \(auto\) CH(\d+)=\d+,(\d+)\.(\d+),[^,]*,1,([^,\r\n]+),')
if($rows.Count -ne 109){throw 'Combined optical space must use modulation profile 1'}
for($i=0;$i -lt $rows.Count;$i++) {
    $frequency=[int]$rows[$i].Groups[2].Value*1000+[int]$rows[$i].Groups[3].Value
    if([int]$rows[$i].Groups[1].Value -ne $i -or $frequency -ne $reference[$i].Frequency){throw "Combined optical channel mismatch: $i"}
}
Write-Output 'PASS terrestrial-combined: UHF space 0 and 109 optical channels in space 1'
