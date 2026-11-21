param([Parameter(Mandatory=$true)][string]$GameDirectory,[int]$Port=27121,[ValidateSet('baseline','full','intro','hammer','campaign','resume','maps','party','cabin','lobby','lamp')][string]$Mode='baseline',[ValidateRange(2,4)][int]$Players=2,[string]$Root='', [string]$Executable='', [int]$OneWayDelayMs=0, [int]$JitterMs=0, [int]$DropEvery=20,[ValidateSet('','host','client')][string]$Hitch='',[ValidateRange(100,1000)][int]$Walk=500,[switch]$ClientSimpleSwing,[string]$PreviousRun="")
$ErrorActionPreference='Stop'
$root=if($Root) { [IO.Path]::GetFullPath($Root) } else { (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path }
$run=Join-Path $root ('build/qa/coop-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $run | Out-Null
$run | Set-Content (Join-Path $root 'build/latest-native-qa.txt')
$env:PENUMBRA_COOP_QA=$Mode
$env:PENUMBRA_COOP_PORT="$Port"
if($OneWayDelayMs -gt 0) { $env:PENUMBRA_COOP_PORT="$($Port+1)" }
$env:PENUMBRA_COOP_QA_HITCH=$Hitch
$env:PENUMBRA_COOP_QA_WALK="$Walk"
$env:PENUMBRA_COOP_RESUME=if($Mode-eq'resume'){'1'}else{'0'}
$env:PENUMBRA_COOP_ROLE='host'
# Lobby prepartita solo nella prova lobby; nomi fissi per i controlli.
$env:PENUMBRA_COOP_LOBBY=if($Mode-eq'lobby'){'1'}else{''}
$env:PENUMBRA_COOP_NAME='QAHost'
# Giocatori in totale (host compreso); i client oltre il primo si collegano
# direttamente all'host, senza il relay di rete degradata.
$env:PENUMBRA_COOP_PLAYERS="$Players"
$hostPort=$env:PENUMBRA_COOP_PORT
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
    $duration=if($Mode-eq'maps'){880}elseif($Mode-eq'campaign'){150}elseif($Mode-eq'hammer'){75}else{55}
    $relay=Start-Process -FilePath $shell -ArgumentList @('-NoProfile','-File', ('"'+$relayScript+'"'),'-Port', "$Port",'-Delay',"$OneWayDelayMs",'-Jitter',"$JitterMs",'-DropEvery',"$DropEvery",'-Duration',"$duration",'-StopFile',('"'+(Join-Path $run 'relay-stop')+'"')) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $run 'relay.log') -RedirectStandardError (Join-Path $run 'relay-error.log') -PassThru
    $null=$relay.Handle
    Start-Sleep -Milliseconds 1500
}
$hostProcess=Start-Process -FilePath $exe -WorkingDirectory $GameDirectory -WindowStyle Hidden -PassThru
$null=$hostProcess.Handle # Windows PowerShell reports a null ExitCode otherwise
$env:PENUMBRA_COOP_ROLE='client'
$env:PENUMBRA_COOP_NAME='QAClient'
$env:PENUMBRA_COOP_QA_SIMPLE=if($ClientSimpleSwing){'1'}else{'0'}
$env:PENUMBRA_COOP_PORT="$Port"
$env:PENUMBRA_COOP_ADDRESS='127.0.0.1'
$env:PENUMBRA_COOP_PROFILE_ROOT=Join-Path $run 'client'
$env:PENUMBRA_COOP_SCREENSHOT=Join-Path $run 'client.bmp'
$clientProcess=Start-Process -FilePath $exe -WorkingDirectory $GameDirectory -WindowStyle Hidden -PassThru
$null=$clientProcess.Handle
$processes=@($hostProcess,$clientProcess)
$roles=@('host','client')
for($n=2;$n-lt$Players;++$n) {
    Start-Sleep -Milliseconds 1500
    $env:PENUMBRA_COOP_PORT=$hostPort
    $env:PENUMBRA_COOP_PROFILE_ROOT=Join-Path $run "client$n"
    $env:PENUMBRA_COOP_SCREENSHOT=Join-Path $run "client$n.bmp"
    $extra=Start-Process -FilePath $exe -WorkingDirectory $GameDirectory -WindowStyle Hidden -PassThru
    $null=$extra.Handle
    $processes+=$extra; $roles+="client$n"
}
@($processes | ForEach-Object Id) | Set-Content (Join-Path $run 'pids.txt')
$deadline=(Get-Date).AddSeconds($(if($Mode-eq'maps'){900}elseif($Mode-eq'party'){260}elseif($Mode-eq'cabin'){185}elseif($Mode-eq'campaign'){180}elseif($Mode-eq'hammer'){95}elseif($Mode-eq'full'){80}else{65}))
while((Get-Date)-lt $deadline -and @($processes | Where-Object { !$_.HasExited }).Count) { Start-Sleep -Milliseconds 250 }
$passed=$true
foreach($process in $processes) {
    if(!$process.HasExited) {
        if($process.Path-ne $exe) { throw 'Unexpected process path' }
        Stop-Process -Id $process.Id -Force; $passed=$false
    }
    $process.Refresh(); Write-Output "PID=$($process.Id) Exit=$($process.ExitCode)"
    if($process.ExitCode-ne0) { $passed=$false }
}
foreach($role in $roles) {
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
