param([Parameter(Mandatory=$true)][string]$GameDirectory,[int]$Port=27121,[ValidateSet('baseline','full','intro','hammer','campaign','resume')][string]$Mode='baseline',[string]$Root='', [string]$Executable='', [int]$OneWayDelayMs=0, [int]$DropEvery=20,[switch]$ClientSimpleSwing,[string]$PreviousRun="")
$ErrorActionPreference='Stop'
$root=if($Root) { [IO.Path]::GetFullPath($Root) } else { (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path }
$run=Join-Path $root ('build/qa/coop-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $run | Out-Null
$run | Set-Content (Join-Path $root 'build/latest-native-qa.txt')
$env:PENUMBRA_COOP_QA=$Mode
$env:PENUMBRA_COOP_PORT="$Port"
if($OneWayDelayMs -gt 0) { $env:PENUMBRA_COOP_PORT="$($Port+1)" }
$env:PENUMBRA_COOP_RESUME=if($Mode-eq'resume'){'1'}else{'0'}
$env:PENUMBRA_COOP_ROLE='host'
$env:PENUMBRA_COOP_PROFILE_ROOT=Join-Path $run 'host'
if($Mode-eq'resume') {
    if(!$PreviousRun) { throw 'Resume QA requires PreviousRun' }
    Copy-Item -LiteralPath (Join-Path $PreviousRun 'host') -Destination (Join-Path $run 'host') -Recurse
}
$env:PENUMBRA_COOP_SCREENSHOT=Join-Path $run 'host.bmp'
$exe=if($Executable) { [IO.Path]::GetFullPath($Executable) } else { Join-Path $root 'build/bin/Release/PenumbraCoop.exe' }
$relay=$null
if($OneWayDelayMs -gt 0) {
    $shell=(Get-Process -Id $PID).Path
    $relayScript=Join-Path $root 'tests/network/Relay.ps1'
    $duration=if($Mode-eq'campaign'){150}elseif($Mode-eq'hammer'){75}else{55}
    $relay=Start-Process -FilePath $shell -ArgumentList @('-NoProfile','-File', ('"'+$relayScript+'"'),'-Port', "$Port",'-Delay',"$OneWayDelayMs",'-DropEvery',"$DropEvery",'-Duration',"$duration",'-StopFile',('"'+(Join-Path $run 'relay-stop')+'"')) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $run 'relay.log') -RedirectStandardError (Join-Path $run 'relay-error.log') -PassThru
    Start-Sleep -Milliseconds 1500
}
$hostProcess=Start-Process -FilePath $exe -WorkingDirectory $GameDirectory -WindowStyle Hidden -PassThru
$env:PENUMBRA_COOP_ROLE='client'
$env:PENUMBRA_COOP_QA_SIMPLE=if($ClientSimpleSwing){'1'}else{'0'}
$env:PENUMBRA_COOP_PORT="$Port"
$env:PENUMBRA_COOP_ADDRESS='127.0.0.1'
$env:PENUMBRA_COOP_PROFILE_ROOT=Join-Path $run 'client'
$env:PENUMBRA_COOP_SCREENSHOT=Join-Path $run 'client.bmp'
$clientProcess=Start-Process -FilePath $exe -WorkingDirectory $GameDirectory -WindowStyle Hidden -PassThru
@($hostProcess.Id,$clientProcess.Id) | Set-Content (Join-Path $run 'pids.txt')
$deadline=(Get-Date).AddSeconds($(if($Mode-eq'campaign'){180}elseif($Mode-eq'hammer'){95}else{65}))
while((Get-Date)-lt $deadline -and (!$hostProcess.HasExited -or !$clientProcess.HasExited)) { Start-Sleep -Milliseconds 250 }
$passed=$true
foreach($process in @($hostProcess,$clientProcess)) {
    if(!$process.HasExited) {
        if($process.Path-ne $exe) { throw 'Unexpected process path' }
        Stop-Process -Id $process.Id -Force; $passed=$false
    }
    $process.Refresh(); Write-Output "PID=$($process.Id) Exit=$($process.ExitCode)"
    if($process.ExitCode-ne0) { $passed=$false }
}
foreach($role in @('host','client')) {
    Write-Output $role
    $log=Join-Path $run "$role/PenumbraOnlineCoop/Overture/hpl.log"
    Select-String -Path $log -Pattern '\[COOP/QA\]|FATAL ERROR|Couldn.t build script' | ForEach-Object Line
    if(!(Select-String -Path $log -Pattern '\[COOP/QA\] PASS' -Quiet)) { $passed=$false }
}
if($relay) {
    [IO.File]::WriteAllText((Join-Path $run 'relay-stop'),'done')
    $relay.WaitForExit(15000) | Out-Null
    if(!$relay.HasExited) { Stop-Process -Id $relay.Id; $passed=$false }
    elseif($relay.ExitCode-ne0) { $passed=$false }
    Get-Content (Join-Path $run 'relay.log')
    Get-Content (Join-Path $run 'relay-error.log')
}
Write-Output $run
if(!$passed) { throw 'Native cooperative test failed; logs retained' }
