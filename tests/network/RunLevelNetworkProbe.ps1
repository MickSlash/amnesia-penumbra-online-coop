# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [ValidateSet('01_old_archives.map')][string]$StartMap = '01_old_archives.map',
    [ValidateRange(1,65534)][int]$Port = 27080,
    [ValidateRange(0,500)][int]$OneWayDelayMs = 35,
    [ValidateRange(2,1000)][int]$DropEvery = 1000,
    [ValidateRange(60,600)][int]$Seconds = 180,
    [switch]$Visual
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$exe = Join-Path $repo 'amnesia/src/game/x64/Release/Lux.exe'
$tag = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$run = Join-Path $repo "build/level-network-probe/$tag"
New-Item -ItemType Directory -Path $run -Force | Out-Null
$culture = [Globalization.CultureInfo]::InvariantCulture
$saved = @{}; $processes = @(); $logs = @{}; $serial = 1; $relay = $null
foreach($name in [Environment]::GetEnvironmentVariables().Keys) {
    if($name.StartsWith('AMNESIA_NET_')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
}
function Read-State([string]$role) {
    $path = Join-Path $run "$role.drive.state"
    if(!(Test-Path -LiteralPath $path)) { return $null }
    try { $fields = [IO.File]::ReadAllText($path).Trim() -split '\s+' } catch [IO.IOException] { return $null }
    if($fields.Count -ne 10) { return $null }
    [pscustomobject]@{epoch=[int]$fields[0];map=$fields[1];x=[float]::Parse($fields[2],$culture);y=[float]::Parse($fields[3],$culture);z=[float]::Parse($fields[4],$culture);yaw=[float]::Parse($fields[5],$culture);pitch=[float]::Parse($fields[6],$culture);active=$fields[7] -eq '1';health=[float]::Parse($fields[8],$culture);focus=$fields[9]}
}
function Command([string]$role,[int]$forward,[int]$strafe,[int]$yaw,[int]$interact=0,[int]$pitch=0) {
    $script:serial++
    for($attempt=0;$attempt -lt 10;$attempt++) {
        try { [IO.File]::WriteAllText((Join-Path $run "$role.drive"),"$script:serial 90 $forward $strafe $yaw $pitch $interact 0"); return }
        catch [IO.IOException] { if($attempt -eq 9) { throw }; Start-Sleep -Milliseconds 10 }
    }
}
try {
    foreach($name in @($saved.Keys)) { [Environment]::SetEnvironmentVariable($name,$null) }
    $relayText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'RunImpairedNetworkTest.ps1'))
    $relayMatch = [regex]::Match($relayText,"(?s)\`$relaySource = @'\r?\n(.*?)\r?\n'@")
    if(!$relayMatch.Success) { throw 'Existing UDP impairment relay source not found' }
    $relaySource = Join-Path $run 'relay.cs'; [IO.File]::WriteAllText($relaySource,$relayMatch.Groups[1].Value)
    $relayScript = Join-Path $run 'relay.ps1'
    @'
param([string]$Source,[int]$Port,[int]$Delay,[int]$DropEvery,[int]$Seconds)
$ErrorActionPreference='Stop'
Add-Type -Path $Source
[CoopUdpRelay]::Run($Port,$Delay,$DropEvery,$Seconds,1200)
'@ | Set-Content -LiteralPath $relayScript -Encoding utf8
    $relay = Start-Process -FilePath (Get-Process -Id $PID).Path -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-File',$relayScript,'-Source',$relaySource,'-Port',$Port,'-Delay',$OneWayDelayMs,'-DropEvery',$DropEvery,'-Seconds',($Seconds+30)) -RedirectStandardOutput "$run/relay.log" -RedirectStandardError "$run/relay.err"
    Start-Sleep -Seconds 2
    if($relay.HasExited) { throw 'UDP relay failed to start' }
    foreach($role in @('host','client')) {
        $profile = "OnlineLevelProbe_${tag}_$role"
        $logs[$role] = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
        $settings = Join-Path $run "$role-settings.cfg"
        $show = if($role -eq 'host') {'false'} else {'true'}
        @"
<Main ShowMenu="$show" ShowPreMenu="false" SaveConfig="false" DefaultProfileName="levelqa" UpdateLogActive="false" />
<Graphics TextureQuality="1" Shadows="1" SSAOActive="false" />
<Engine LimitFPS="true" />
<Screen Width="960" Height="720" Display="0" FullScreen="false" Vsync="false" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
"@ | Set-Content -LiteralPath $settings -Encoding utf8
        $init = [IO.File]::ReadAllText((Join-Path $game 'config/main_init.cfg'))
        $init = $init -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        $init = $init -replace 'GameName\s*=\s*"[^"]*"', "GameName = `"Amnesia Level Network QA [$role]`""
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settings`""
        $init = $init -replace 'File\s*=\s*"00_rainy_hall.map"', "File = `"$StartMap`""
        $config = Join-Path $run "$role-init.cfg"; [IO.File]::WriteAllText($config,$init)
        $drive = Join-Path $run "$role.drive"; [IO.File]::WriteAllText($drive,'1 0 0 0 0 0 0 0')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE',$role)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT',[string]$(if($role -eq 'host'){$Port+1}else{$Port}))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS','127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS',[string]$Seconds)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_DRIVE_FILE',$drive)
        $style = if($Visual) {'Normal'} else {'Hidden'}
        $process = Start-Process -FilePath $exe -WorkingDirectory $game -ArgumentList $config -WindowStyle $style -PassThru
        $processes += $process
        [pscustomobject]@{role=$role;pid=$process.Id;log=$logs[$role]} | ConvertTo-Json | Set-Content -LiteralPath "$run/$role-process.json"
    }
    [pscustomobject]@{map=$StartMap;executableSha256=(Get-FileHash $exe -Algorithm SHA256).Hash;nominalRttMs=2*$OneWayDelayMs;dropEvery=$DropEvery;seconds=$Seconds;visual=$Visual.IsPresent} | ConvertTo-Json | Set-Content -LiteralPath "$run/manifest.json"
    Write-Output "Running native level probe: $run"
    $start = [DateTime]::UtcNow; $goalAt = $start; $point = 0; $blocked = @(); $reached = @(); $movementStarted = $false
    # Ordinary engine input only: no teleports, direct script callbacks or injected fixtures.
    # This probes the opening corridors/events, rather than certifying the whole level.
    $route = @(@(-1,9),@(-1,13),@(-1,21),@(-8,21),@(-8,24),@(-1,21),@(8,21),@(8,23),@(8,27),@(8,19),@(8,12))
    $overridePath = Join-Path $run 'override.goal'; [IO.File]::WriteAllText($overridePath,'')
    while(@($processes | Where-Object {!$_.HasExited}).Count -and ([DateTime]::UtcNow-$start).TotalSeconds -lt ($Seconds+15)) {
        $age = ([DateTime]::UtcNow-$start).TotalSeconds
        $state = Read-State 'client'; $hostState = Read-State 'host'
        if($state -and $hostState) {
            [pscustomobject]@{timeUtc=[DateTime]::UtcNow.ToString('o');elapsedSeconds=$age;waypoint=$point;host=$hostState;client=$state} | ConvertTo-Json -Depth 4 -Compress | Add-Content -LiteralPath "$run/trace.jsonl"
        }
        if($state -and $hostState -and $age -gt 10 -and $state.map.EndsWith($StartMap) -and $state.active) {
            if(!$movementStarted) { $goalAt=[DateTime]::UtcNow; $movementStarted=$true }
            $override = [IO.File]::ReadAllText($overridePath).Trim()
            $goal = if($point -lt $route.Count) {$route[$point]} else {$null}
            if($override -eq 'stop') { $goal=$null }
            elseif($override) {
                $parts=$override -split '\s+'; if($parts.Count -ne 2) {throw 'override.goal expects x z or stop'}
                $goal=@([float]::Parse($parts[0],$culture),[float]::Parse($parts[1],$culture)); $goalAt=[DateTime]::UtcNow
            }
            $yaw = [int]([Math]::Atan2([Math]::Sin($state.yaw),[Math]::Cos($state.yaw))*1000)
            $forward=0
            if($goal) {
                $dx=$goal[0]-$state.x; $dz=$goal[1]-$state.z; $distance=[Math]::Sqrt($dx*$dx+$dz*$dz)
                if($distance -lt 0.6 -and !$override) {
                    $reached += $point; Write-Output "Reached waypoint $point ($($goal -join ','))"; $point++; $goalAt=[DateTime]::UtcNow
                } elseif(!$override -and ([DateTime]::UtcNow-$goalAt).TotalSeconds -gt 25) {
                    $blocked += $point; Write-Output "Waypoint $point obstructed/timed out at $($state.x),$($state.z), focus=$($state.focus)"; $point++; $goalAt=[DateTime]::UtcNow
                } elseif($distance -ge 0.6) {
                    $yaw=[int]([Math]::Atan2(-$dx,-$dz)*1000)
                    $turn=[Math]::Atan2([Math]::Sin($yaw/1000.0-$state.yaw),[Math]::Cos($yaw/1000.0-$state.yaw))
                    if([Math]::Abs($turn) -lt 0.3) {$forward=[int][Math]::Min(700,$distance*350)}
                }
            }
            Command 'client' $forward 0 $yaw
        }
        Start-Sleep -Milliseconds 250
    }
    foreach($process in $processes) { $process.Refresh(); if(!$process.HasExited -or $process.ExitCode -ne 0) { throw 'Level probe process failed or timed out' } }
    foreach($role in @('host','client')) {
        $log=[IO.File]::ReadAllText($logs[$role]); [IO.File]::WriteAllText("$run/$role.log",$log)
        foreach($marker in @('Protocol handshake accepted','Smoke test finished')) {if(!$log.Contains($marker)){throw "Missing $role proof: $marker"}}
        if($log.Contains('Rejected:') -or $log.Contains('Invalid native save schema') -or $log.Contains('closing session')) {throw "$role session/schema failure"}
        if(([regex]::Matches($log,'Protocol handshake accepted')).Count -ne 1){throw "$role unexpectedly reconnected"}
    }
    $clientLog=[IO.File]::ReadAllText("$run/client.log")
    if(!$clientLog.Contains('Complete host entity baseline applied') -or !$clientLog.Contains('Complete campaign checkpoint received')) {throw 'Incomplete client baseline/checkpoint'}
    $intervals = @([regex]::Matches($clientLog,'Player state intervals average_ms=(\d+) max_ms=(\d+) samples=(\d+)') | ForEach-Object {
        [pscustomobject]@{averageMs=[int]$_.Groups[1].Value;maxMs=[int]$_.Groups[2].Value;samples=[int]$_.Groups[3].Value}
    })
    [pscustomobject]@{transportPassed=$true;reachedWaypoints=$reached;obstructedWaypoints=$blocked;wholeLevelCompleted=$false;clientIntervalWindows=$intervals;clientCheckpointCount=[regex]::Matches($clientLog,'Complete campaign checkpoint received').Count} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$run/result.json"
    if(!$relay.WaitForExit(30000)) {throw 'Relay timeout'}
    $relay.Refresh(); if($relay.ExitCode -ne 0){throw 'Relay failure'}
    Get-Content -LiteralPath "$run/relay.log"
    Write-Output "PASS: native level session, baseline and checkpoints; route coverage recorded separately: $run"
} finally {
    foreach($process in $processes) {if(!$process.HasExited){Stop-Process -Id $process.Id; [void]$process.WaitForExit(3000)}}
    foreach($role in @('host','client')) {if($logs.ContainsKey($role) -and (Test-Path -LiteralPath $logs[$role])){Copy-Item -LiteralPath $logs[$role] -Destination "$run/$role.log" -Force}}
    if($relay -and !$relay.HasExited) {Stop-Process -Id $relay.Id; [void]$relay.WaitForExit(3000)}
    foreach($name in @([Environment]::GetEnvironmentVariables().Keys)) {if($name.StartsWith('AMNESIA_NET_')){[Environment]::SetEnvironmentVariable($name,$null)}}
    foreach($name in $saved.Keys) {[Environment]::SetEnvironmentVariable($name,$saved[$name])}
}
