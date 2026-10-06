# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$Executable="$PSScriptRoot/../../build/network/NetworkTests.exe")
$ErrorActionPreference='Stop'
$exe=(Resolve-Path -LiteralPath $Executable).Path
$run=Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path ('build/crash-probe/'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$probe=Start-Process -FilePath $exe -ArgumentList @('--crash-probe',"`"$run`"") -WindowStyle Hidden -PassThru
if(!$probe.WaitForExit(15000)) {Stop-Process -Id $probe.Id; throw 'Crash probe timed out'}
$probe.Refresh()
if($probe.ExitCode -eq 0) {throw 'Crash probe unexpectedly exited normally'}
$report=Get-ChildItem -LiteralPath $run -Filter "coop-crash-*-$($probe.Id).txt" | Select-Object -First 1
if(!$report) {throw 'Missing report for the current probe process'}
$text=Get-Content -LiteralPath $report.FullName -Raw
if($text -notmatch 'exception=0xe0424242' -or $text -notmatch 'dump_written=1' -or $text -notmatch 'module=') {throw 'Incomplete crash report'}
$dump=[IO.Path]::ChangeExtension($report.FullName,'.dmp')
if(!(Test-Path -LiteralPath $dump) -or (Get-Item -LiteralPath $dump).Length -lt 1000) {throw 'Missing or empty minidump'}
Write-Output "PASS: controlled exception writes text report and minidump ($run)"
