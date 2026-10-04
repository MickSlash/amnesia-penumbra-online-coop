# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [int]$Port = 27082,
    [ValidateRange(0,200)][int]$OneWayDelayMs = 35,
    [ValidateRange(2,1000)][int]$DropEvery = 20,
    [switch]$Visual
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$exe = Join-Path $repo 'amnesia/src/game/x64/Release/Lux.exe'
$tag = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$run = Join-Path $repo "build/campaign-start/$tag"
New-Item -ItemType Directory -Path $run -Force | Out-Null
$saved = @{}; $processes = @(); $logs = @{}; $relay = $null
foreach($name in [Environment]::GetEnvironmentVariables().Keys) {
    if($name.StartsWith('AMNESIA_NET_')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
}
function Read-State([string]$role) {
    $path = Join-Path $run "$role.drive.state"
    if(!(Test-Path -LiteralPath $path)) { return $null }
    try { $fields = [IO.File]::ReadAllText($path).Trim() -split '\s+' } catch [IO.IOException] { return $null }
    if($fields.Count -ne 10) { return $null }
    $culture=[Globalization.CultureInfo]::InvariantCulture
    [pscustomobject]@{x=[float]::Parse($fields[2],$culture); z=[float]::Parse($fields[4],$culture); active=$fields[7] -eq '1'}
}
function Read-LiveLog([string]$path) {
    if(!(Test-Path -LiteralPath $path)) { return '' }
    $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=[IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
try {
    foreach($name in @($saved.Keys)) { [Environment]::SetEnvironmentVariable($name,$null) }
    $relayText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'RunImpairedNetworkTest.ps1'))
    $relayMatch = [regex]::Match($relayText,"(?s)\`$relaySource = @'\r?\n(.*?)\r?\n'@")
    if(!$relayMatch.Success) { throw 'Existing UDP impairment relay source not found' }
    $relaySource = Join-Path $run 'relay.cs'; [IO.File]::WriteAllText($relaySource,$relayMatch.Groups[1].Value)
    $relayScript = Join-Path $run 'relay.ps1'
    @'
param([string]$Source,[int]$Port,[int]$Delay,[int]$DropEvery)
$ErrorActionPreference='Stop'
Add-Type -Path $Source
[CoopUdpRelay]::Run($Port,$Delay,$DropEvery,205,1200)
'@ | Set-Content -LiteralPath $relayScript -Encoding utf8
    $relay = Start-Process -FilePath (Get-Process -Id $PID).Path -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-File',$relayScript,'-Source',$relaySource,'-Port',$Port,'-Delay',$OneWayDelayMs,'-DropEvery',$DropEvery) -RedirectStandardOutput "$run/relay.log" -RedirectStandardError "$run/relay.err"
    Start-Sleep -Seconds 2
    foreach($role in @('host','client')) {
        $profile = "OnlineCampaignStart_${tag}_$role"
        $logs[$role] = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
        $settings = Join-Path $run "$role-settings.cfg"
        @'
<Main ShowMenu="true" ShowPreMenu="false" SaveConfig="false" DefaultProfileName="introqa" UpdateLogActive="false" />
<Graphics TextureQuality="1" Shadows="1" SSAOActive="false" />
<Engine LimitFPS="true" />
<Screen Width="960" Height="720" Display="0" FullScreen="false" Vsync="false" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
<Game ShowSubtitles="true" />
'@ | Set-Content -LiteralPath $settings -Encoding utf8
        $init = [IO.File]::ReadAllText((Join-Path $game 'config/main_init.cfg'))
        $init = $init -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        $init = $init -replace 'GameName\s*=\s*"[^"]*"', "GameName = `"Amnesia Campaign Start QA [$role]`""
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settings`""
        $config = Join-Path $run "$role-init.cfg"; [IO.File]::WriteAllText($config,$init)
        $drive = Join-Path $run "$role.drive"; [IO.File]::WriteAllText($drive,'1 0 0 0 0 0 0 0')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE',$role)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT',[string]$(if($role -eq 'host'){$Port+1}else{$Port}))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS','127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS','180')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_CAMPAIGN_START','1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_DRIVE_FILE',$drive)
        $style = if($Visual) {'Normal'} else {'Hidden'}
        $process = Start-Process -FilePath $exe -WorkingDirectory $game -ArgumentList $config -WindowStyle $style -PassThru
        $processes += $process
        [pscustomobject]@{role=$role;pid=$process.Id;log=$logs[$role]} | ConvertTo-Json | Set-Content -LiteralPath "$run/$role-process.json"
    }
    [pscustomobject]@{executableSha256=(Get-FileHash $exe -Algorithm SHA256).Hash;nominalRttMs=2*$OneWayDelayMs;dropEvery=$DropEvery;clientExtraLoadSeconds=6;start='OnlineNewCampaign';visual=$Visual.IsPresent} | ConvertTo-Json | Set-Content -LiteralPath "$run/manifest.json"
    Write-Output "Running native lobby/intro regression: $run"
    $deadline = [DateTime]::UtcNow.AddSeconds(155)
    do {
        if($processes | Where-Object HasExited) { throw 'Game exited before completing intro' }
        $hostLog = Read-LiveLog $logs.host
        if($hostLog.Contains('Original campaign intro completed; native timers released controls')) { break }
        Start-Sleep -Milliseconds 500
    } while([DateTime]::UtcNow -lt $deadline)
    if(!$hostLog.Contains('Original campaign intro completed; native timers released controls')) { throw 'Original campaign intro never released controls' }
    Start-Sleep -Seconds 2
    $before = @{}
    foreach($role in @('host','client')) {
        $before[$role]=Read-State $role
        if(!$before[$role] -or !$before[$role].active) { throw "$role controls still locked after original intro" }
        # The existing QA driver feeds ordinary native movement and network input.
        [IO.File]::WriteAllText((Join-Path $run "$role.drive"),'2 90 -1000 0 0 0 0 0')
    }
    Start-Sleep -Seconds 3
    foreach($role in @('host','client')) {
        $after=Read-State $role
        if(!$after) { throw "$role missing movement telemetry" }
        $distance=[Math]::Sqrt([Math]::Pow($after.x-$before[$role].x,2)+[Math]::Pow($after.z-$before[$role].z,2))
        if($distance -lt 0.05) { throw "$role failed ordinary movement after intro: $distance metres" }
        Write-Output "PASS: $role moves after original intro ($([Math]::Round($distance,3)) m)"
        [pscustomobject]@{role=$role;before=$before[$role];after=$after;distanceMetres=$distance} | ConvertTo-Json | Set-Content -LiteralPath "$run/$role-movement.json"
    }
    $exitDeadline=[DateTime]::UtcNow.AddSeconds(190)
    while(@($processes | Where-Object { !$_.HasExited }).Count -gt 0 -and [DateTime]::UtcNow -lt $exitDeadline) { Start-Sleep -Milliseconds 500 }
    foreach($process in $processes) { $process.Refresh(); if(!$process.HasExited -or $process.ExitCode -ne 0) { throw 'Game did not exit cleanly' } }
    foreach($role in @('host','client')) {
        $log=[IO.File]::ReadAllText($logs[$role]); [IO.File]::WriteAllText((Join-Path $run "$role.log"),$log)
        foreach($voice in 1..4) {
            $asset='CH01L00_DanielsMind{0:d2}_01' -f $voice
            if(!($log -match ('Campaign intro voice playing asset='+[regex]::Escape($asset)+' subtitles=1'))) { throw "$role missing native intro audio/subtitle proof: $asset" }
        }
        if(([regex]::Matches($log,'Protocol handshake accepted')).Count -ne 1 -or $log.Contains('Connection lost') -or $log.Contains('Rejected:') -or $log.Contains('FAILED') -or $log.Contains('readiness timed out')) { throw "$role disconnected or failed during intro" }
        $checkpointProof=if($role -eq 'host'){'Captured campaign checkpoint'}else{'Complete campaign checkpoint received'}
        if(!$log.Contains('Complete host entity baseline') -or !$log.Contains($checkpointProof)) { throw "$role missing campaign baseline/checkpoint" }
        if($role -eq 'client' -and !$log.Contains('Client shares original intro camera with host; companion avatars suppressed')) { throw 'Client did not share the original intro camera' }
        Write-Output "PASS: $role four original intro voices/subtitles, baseline, checkpoint and single uninterrupted connection"
    }
    if(!$relay.WaitForExit(30000)) { throw 'Relay did not finish on its timer' }
    Get-Content -LiteralPath "$run/relay.log"
} finally {
    foreach($role in @('host','client')) { if($logs[$role] -and (Test-Path -LiteralPath $logs[$role])) { [IO.File]::WriteAllText("$run/$role.log",(Read-LiveLog $logs[$role])) } }
    foreach($process in $processes) { if(!$process.HasExited) { Stop-Process -Id $process.Id } }
    if($relay -and !$relay.HasExited) { Stop-Process -Id $relay.Id }
    foreach($name in @([Environment]::GetEnvironmentVariables().Keys)) {if($name.StartsWith('AMNESIA_NET_')){[Environment]::SetEnvironmentVariable($name,$null)}}
    foreach($name in $saved.Keys) {[Environment]::SetEnvironmentVariable($name,$saved[$name])}
}
