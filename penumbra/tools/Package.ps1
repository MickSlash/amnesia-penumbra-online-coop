# SPDX-License-Identifier: GPL-3.0-or-later
param([Parameter(Mandatory=$true)][string]$EvidenceFile)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$gitRoot=$root.Replace('\','/')
$revision=(& git -c "safe.directory=$gitRoot" -C $root rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0) {throw 'Commit the source before packaging'}
$changes=& git -c "safe.directory=$gitRoot" -C $root status --porcelain
if($LASTEXITCODE-ne0 -or $changes) {throw 'Source changes must be committed before packaging'}
$exe=Join-Path $root 'build/package-candidate/bin/PenumbraCoop.exe'
$launcher=Join-Path $root 'build/package-candidate/PenumbraCoopLauncher.exe'
$evidence=Get-Content -LiteralPath $EvidenceFile -Raw | ConvertFrom-Json
if($evidence.game_sha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash) {throw 'Test evidence does not match the game binary'}
if($evidence.launcher_sha256 -ne (Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash) {throw 'Test evidence does not match the launcher'}
if(!$evidence.protocol_pass -or !$evidence.native_full_pass -or !$evidence.native_intro_pass -or !$evidence.native_hammer_pass -or !$evidence.native_hammer_impaired_pass -or !$evidence.steam_presence_pass -or !$evidence.native_campaign_pass -or !$evidence.native_campaign_impaired_pass -or !$evidence.native_resume_pass) {throw 'Required prototype checks have not passed'}
$name='PenumbraOnlineCoop_0.4_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + $revision.Substring(0,8)
$packages=Join-Path $root 'packages'
$release=Join-Path $packages $name
New-Item -ItemType Directory -Path (Join-Path $release 'bin') -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $release 'bin/PenumbraCoop.exe')
Copy-Item -LiteralPath $launcher -Destination (Join-Path $release 'PenumbraCoopLauncher.exe')
foreach($file in @('README.md','BUILD.md','TESTS.md','THIRD_PARTY.md','LICENSE','upstream.json')) {
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $release $file)
}
Copy-Item -LiteralPath $EvidenceFile -Destination (Join-Path $release 'validation.json')
$source=Join-Path $release 'PenumbraOnlineCoop-source.zip'
& git -c "safe.directory=$gitRoot" -C $root archive --format=zip "--output=$source" HEAD
if($LASTEXITCODE-ne0) {throw 'Source archive failed'}
$hashes=@()
Get-ChildItem -LiteralPath $release -File -Recurse | ForEach-Object {
    $hashes+=[ordered]@{file=$_.FullName.Substring($release.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash; bytes=$_.Length}
}
$manifest=[ordered]@{version='0.4-experimental'; protocol_version=6; source_commit=$revision; created_utc=[DateTime]::UtcNow.ToString('o'); full_campaign_certified=$false; files=$hashes}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $release 'manifest.json') -Encoding UTF8
$zip=Join-Path $packages ($name+'.zip')
Compress-Archive -LiteralPath $release -DestinationPath $zip
Write-Output $zip
Write-Output $release
Get-FileHash -LiteralPath $zip -Algorithm SHA256
