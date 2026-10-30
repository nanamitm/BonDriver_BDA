[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TVTestDirectory,
    [string]$Subdirectory = '4k8kdrver-direct'
)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$tvtest=(Resolve-Path -LiteralPath $TVTestDirectory).Path
if(Get-CimInstance Win32_Process | Where-Object {$_.ExecutablePath -eq (Join-Path $tvtest 'TVTest.exe')}) {
    throw 'Close this TVTest installation before deploying.'
}
$destination=[IO.Path]::GetFullPath((Join-Path $tvtest $Subdirectory))
if(!$destination.StartsWith($tvtest.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Target directory must be within the TVTest installation.'
}
$dll=Join-Path $destination 'TBS6812.dll'
$ini=Join-Path $destination 'BonDriver_BDA_4k.ini'
$source=Join-Path $repo 'x64\Release_Static\TBS6812.dll'
foreach($file in $dll,$ini,$source){if(!(Test-Path -LiteralPath $file)){throw "Missing file: $file"}}
$cache=Join-Path $destination 'warm-start-cache'
$backup=Join-Path $repo ('work\deployment-backups\warm-start-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force $backup | Out-Null
$originals=foreach($file in $dll,$ini){
    $saved=Join-Path $backup (Split-Path $file -Leaf)
    Copy-Item -LiteralPath $file -Destination $saved
    $hash=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if((Get-FileHash -LiteralPath $saved -Algorithm SHA256).Hash -ne $hash){throw "Backup hash mismatch: $file"}
    [pscustomobject]@{Path=$file;Backup=$saved;SHA256=$hash}
}
$unchanged=foreach($file in 'BonDriver_dantto4k.dll','BonDriver_dantto4k.ini',($Subdirectory+'\BonDriver_BDA_4k.dll')){
    $path=Join-Path $tvtest $file
    [pscustomobject]@{Path=$path;SHA256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
}
$encoding=[Text.UTF8Encoding]::new($false,$true)
$bytes=[IO.File]::ReadAllBytes($ini)
try{$text=$encoding.GetString($bytes)}catch{$encoding=[Text.Encoding]::GetEncoding(932);$text=$encoding.GetString($bytes)}
function Set-IniValue([string]$content,[string]$key,[string]$value){
    $section=[regex]::Matches($content,'(?ims)^(?<header>\[TBS6812\][^\r\n]*\r?\n)(?<body>.*?)(?=^\[|\z)')
    if($section.Count -ne 1){throw 'Expected one existing TBS6812 section.'}
    $match=$section[0]
    $body=$match.Groups['body'].Value
    $pattern='(?mi)^[ \t]*'+[regex]::Escape($key)+'[ \t]*=[^\r\n]*'
    $found=[regex]::Matches($body,$pattern)
    if($found.Count -gt 1){throw "Duplicate INI setting: $key"}
    if($found.Count -eq 1){$body=[regex]::Replace($body,$pattern,[Text.RegularExpressions.MatchEvaluator]{param($m) $key+'='+$value})}
    else{$body=$body.TrimEnd("`r","`n")+"`r`n"+$key+'='+$value+"`r`n`r`n"}
    return $content.Substring(0,$match.Index)+$match.Groups['header'].Value+$body+$content.Substring($match.Index+$match.Length)
}
$text=Set-IniValue $text 'ExperimentalWarmStart' '1'
$text=Set-IniValue $text 'WarmStartCacheDirectory' $cache
$text=Set-IniValue $text 'InspectWarmState' '0'
try{
    New-Item -ItemType Directory -Force $cache | Out-Null
    Copy-Item -LiteralPath $source -Destination $dll
    if((Get-FileHash $source).Hash -ne (Get-FileHash $dll).Hash){throw 'DLL deployment hash mismatch.'}
    [IO.File]::WriteAllBytes($ini,$encoding.GetBytes($text))
    if([IO.File]::ReadAllText($ini,$encoding) -ne $text){throw 'INI verification failed.'}
    foreach($item in $unchanged){if((Get-FileHash -LiteralPath $item.Path).Hash -ne $item.SHA256){throw "Unexpected file change: $($item.Path)"}}
}catch{
    foreach($item in $originals){Copy-Item -LiteralPath $item.Backup -Destination $item.Path}
    throw
}
$manifest=[pscustomobject]@{
    TVTest=$tvtest;Destination=$destination;Cache=$cache;Backup=$backup
    SourceCommit=(& git -C $repo rev-parse HEAD)
    OriginalFiles=@($originals);PreservedFiles=@($unchanged)
    DeployedFiles=@(foreach($file in $dll,$ini){[pscustomobject]@{Path=$file;SHA256=(Get-FileHash -LiteralPath $file).Hash}})
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding utf8
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $repo 'work\warm-production-deployment.json') -Encoding utf8
$manifest
