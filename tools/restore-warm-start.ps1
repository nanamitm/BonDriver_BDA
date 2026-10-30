[CmdletBinding()]
param([Parameter(Mandatory)][string]$BackupDirectory)
$ErrorActionPreference='Stop'
$backup=(Resolve-Path -LiteralPath $BackupDirectory).Path
$manifest=Get-Content -LiteralPath (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
$tvtest=[IO.Path]::GetFullPath($manifest.TVTest)
$destination=[IO.Path]::GetFullPath($manifest.Destination)
if(!$destination.StartsWith($tvtest.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid target directory in backup manifest.'}
if(Get-CimInstance Win32_Process | Where-Object {$_.ExecutablePath -eq (Join-Path $tvtest 'TVTest.exe')}){throw 'Close this TVTest installation before restoring.'}
if(@($manifest.OriginalFiles).Count -ne 2){throw 'Expected the two warm-start deployment backups.'}
$files=foreach($item in $manifest.OriginalFiles){
    $source=[IO.Path]::GetFullPath($item.Backup)
    $target=[IO.Path]::GetFullPath($item.Path)
    if(!$source.StartsWith($backup.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Backup source is outside the selected backup directory.'}
    if(@('TBS6812.dll','BonDriver_BDA_4k.ini') -notcontains (Split-Path $target -Leaf)){throw 'Unexpected restore filename.'}
    if([IO.Path]::GetDirectoryName($target) -ne $destination){throw 'Restore target is outside the selected installation directory.'}
    if((Get-FileHash -LiteralPath $source).Hash -ne $item.SHA256){throw "Backup hash mismatch: $source"}
    [pscustomobject]@{Source=$source;Target=$target;SHA256=$item.SHA256}
}
foreach($file in $files){
    Copy-Item -LiteralPath $file.Source -Destination $file.Target
    if((Get-FileHash -LiteralPath $file.Target).Hash -ne $file.SHA256){throw "Restored hash mismatch: $($file.Target)"}
    "Restored: $($file.Target)"
}
