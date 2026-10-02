# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [int]$Port = 27060,
    [switch]$KeepOpen,
    [switch]$Resume,
    [ValidateRange(1,100)][int]$StartWaypoint = 1
)
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$run = Join-Path $repo 'build/level-playthrough'
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$exe = Join-Path $repo 'amnesia/src/game/x64/Release/Lux.exe'
New-Item -ItemType Directory -Force -Path $run | Out-Null
$tag = [Guid]::NewGuid().ToString('N').Substring(0,8)
$baseInit = [IO.File]::ReadAllText((Join-Path $game 'config/main_init.cfg'))
$processes = @()
$serial = 1
if($Resume) {
    foreach($role in @('host','client')) {
        $previous=([IO.File]::ReadAllText((Join-Path $run "$role.drive")) -split '\s+')[0]
        $serial=[Math]::Max($serial,[int]$previous+1000)
    }
}
$names = @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_ADDRESS','AMNESIA_NET_SMOKE_SECONDS','AMNESIA_NET_TEST_MOVEMENT','AMNESIA_NET_TEST_PICKUP','AMNESIA_NET_TEST_CAMPAIGN','AMNESIA_NET_TEST_ENEMY','AMNESIA_NET_TEST_DRIVE_FILE')
$saved = @{}
foreach($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
function Read-State([string]$role) {
    $path = Join-Path $run "$role.drive.state"
    if(!(Test-Path -LiteralPath $path)) { return $null }
    if(([DateTime]::UtcNow-(Get-Item -LiteralPath $path).LastWriteTimeUtc).TotalSeconds -gt 3) { return $null }
    $text=$null
    for($attempt=0;$attempt -lt 10;$attempt++) {
        try {$text=[IO.File]::ReadAllText($path);break} catch [IO.IOException] {Start-Sleep -Milliseconds 10}
    }
    if($null -eq $text) {return $null}
    $fields = $text.Trim() -split '\s+'
    if($fields.Count -ne 10) { return $null }
    [pscustomobject]@{epoch=[int]$fields[0];map=$fields[1];x=[float]::Parse($fields[2],$culture);y=[float]::Parse($fields[3],$culture);z=[float]::Parse($fields[4],$culture);yaw=[float]::Parse($fields[5],$culture);pitch=[float]::Parse($fields[6],$culture);active=$fields[7] -eq '1';health=[float]::Parse($fields[8],$culture);focus=$fields[9]}
}
function Command([string]$role,[int]$forward,[int]$yaw,[int]$pitch=0,[int]$interact=0) {
    $script:serial++
    # Every command expires after at most 120 simulation ticks.
    for($attempt=0;$attempt -lt 10;$attempt++) {
        try { [IO.File]::WriteAllText((Join-Path $run "$role.drive"), "$script:serial 60 $forward 0 $yaw $pitch $interact");return }
        catch [IO.IOException] {if($attempt -eq 9){throw};Start-Sleep -Milliseconds 10}
    }
}
function Sample([string]$phase) {
    foreach($role in @('host','client')) {
        $state = Read-State $role
        if($state) { [pscustomobject]@{time=[DateTime]::UtcNow.ToString('o');phase=$phase;role=$role;state=$state} | ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $run 'trace.jsonl') }
    }
}
try {
    if(!$Resume) {
      [IO.File]::WriteAllText((Join-Path $run 'finish.flag'),'pending')
      [IO.File]::WriteAllText((Join-Path $run 'override.goal'),'')
      [IO.File]::WriteAllText((Join-Path $run 'result.json'),'{"completed":false,"status":"running"}')
      [IO.File]::WriteAllText((Join-Path $run 'trace.jsonl'),'')
      foreach($role in @('host','client')) {
        $settings = Join-Path $run "$role-settings.cfg"
        $show = if($role -eq 'host') {'false'} else {'true'}
        @"
<Main ShowMenu="$show" ShowPreMenu="false" SaveConfig="false" DefaultProfileName="levelqa" UpdateLogActive="false" />
<Graphics TextureQuality="1" Shadows="1" SSAOActive="false" />
<Engine LimitFPS="true" />
<Screen Width="960" Height="720" Display="0" FullScreen="false" Vsync="false" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
"@ | Set-Content -LiteralPath $settings -Encoding utf8
        $profile = "OnlineLevelQA_${tag}_$role"
        $init = $baseInit -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        $init = $init -replace 'GameName\s*=\s*"[^"]*"', "GameName = `"Amnesia Level Playthrough [$role]`""
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settings`""
        $initPath = Join-Path $run "$role-init.cfg"
        $init | Set-Content -LiteralPath $initPath -Encoding utf8
        foreach($name in $names) { [Environment]::SetEnvironmentVariable($name,$null) }
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE',$role)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT',[string]$Port)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS','127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS','900')
        $drive = Join-Path $run "$role.drive"
        [IO.File]::WriteAllText($drive,'1 0 0 0 0 0 0')
        # Clear stale telemetry without removing files.
        [IO.File]::WriteAllText(($drive+'.state'),'')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_DRIVE_FILE',$drive)
        $p = Start-Process -FilePath $exe -ArgumentList $initPath -WorkingDirectory $game -WindowStyle Normal -PassThru
        $processes += $p
        [pscustomobject]@{role=$role;pid=$p.Id;log=(Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log")} | ConvertTo-Json | Set-Content (Join-Path $run "$role-process.json")
      }
    }
    $began = [DateTime]::UtcNow
    $ready = $Resume.IsPresent
    while(!$ready -and ([DateTime]::UtcNow-$began).TotalSeconds -lt 180) {
        $h=Read-State 'host'; $c=Read-State 'client'
        if($h -and $c -and $h.active -and $c.active -and [Math]::Abs($h.x) -lt 4 -and [Math]::Abs($h.z) -lt 5 -and ([DateTime]::UtcNow-$began).TotalSeconds -gt 65) { $ready=$true; break }
        Start-Sleep -Milliseconds 250
    }
    if(!$ready) { throw 'Original introduction did not reach an active starting position on both peers.' }
    Write-Output $(if($Resume) {'Resuming existing QA game with normal movement.'} else {'Original introduction finished on host; both players active. Starting normal movement.'})
    # Corridor bends and stair landings; no teleport, script callback, or direct ChangeMap.
    $route = @(@(-1,-6),@(-1,-13),@(11.5,-13),@(11.5,-32.75),@(12,-31.7),@(15,-31.7),@(17,-32.75),@(27,-32.75),@(30.5,-32.75),@(30.5,-28),@(32,-26),@(39.75,-26),@(39.75,-33),@(53,-33),@(53,-13.25),@(57,-13.25),@(71.4,-13.25),@(71.4,-10.6))
    $point=0
    foreach($goal in $route) {
        $point++
        if($point -lt $StartWaypoint) {continue}
        if($point -eq 5) {
            # The original castle_3 door must be grabbed, rather than crossed
            # by an injected transform or a script SetSwingDoorClosed call.
            $until=[DateTime]::UtcNow.AddSeconds(15)
            while([DateTime]::UtcNow -lt $until) {
                $state=Read-State 'client'
                if(!$state) {Start-Sleep -Milliseconds 100;continue}
                $dx=12.2-$state.x;$dz=-33.0-$state.z
                $distance=[Math]::Sqrt($dx*$dx+$dz*$dz)
                $yaw=[int]([Math]::Atan2(-$dx,-$dz)*1000)
                Command 'client' $(if($distance -gt 0.4){[int][Math]::Min(700,$distance*400)}else{0}) $yaw
                if($distance -le 0.4) {break}
                Start-Sleep -Milliseconds 250
            }
            Command 'client' 0 -2350
            Start-Sleep -Seconds 1
            $state=Read-State 'client'
            if(!$state -or $state.focus -ne 'castle_3') {throw 'Client cannot focus the original first door.'}
            for($step=0;$step -lt 24;$step++) {
                $grab=if($step -ge 5 -and $step -lt 20){1}else{0}
                $forward=if($step -ge 8 -and $step -lt 20){400}else{0}
                Command 'client' $forward -2350 0 $grab
                Sample 'original-first-door'
                Start-Sleep -Milliseconds 250
            }
            Write-Output 'Client sent normal grab/push/release actions to the original first door.'
        }
        $deadline=[DateTime]::UtcNow.AddSeconds(75); $reached=$false
        while([DateTime]::UtcNow -lt $deadline) {
            $all=$true
            $overrideText=[IO.File]::ReadAllText((Join-Path $run 'override.goal')).Trim()
            if($overrideText -eq 'stop') {throw 'QA navigation paused by operator; game state retained with KeepOpen.'}
            $navigationGoal=$goal
            if($overrideText) {
                $parts=$overrideText -split '\s+'
                if($parts.Count -ne 2) {throw 'override.goal needs exactly x z.'}
                $navigationGoal=@([float]::Parse($parts[0],$culture),[float]::Parse($parts[1],$culture))
                $deadline=[DateTime]::UtcNow.AddSeconds(75)
            }
            foreach($role in @('host','client')) {
                $state=Read-State $role
                if(!$state -or $state.map -notmatch '00_rainy_hall') { throw "Unexpected map for $role before original exit interaction." }
                $dx=$navigationGoal[0]-$state.x; $dz=$navigationGoal[1]-$state.z
                # Align to the narrow entrance before walking along its length.
                if($point -eq 4 -and !$overrideText -and [Math]::Abs($dx) -gt 0.4) {$dz=0}
                $distance=[Math]::Sqrt($dx*$dx+$dz*$dz)
                $yaw=[int]([Math]::Atan2(-$dx,-$dz)*1000)
                $forward=if($distance -gt 0.7) {[int][Math]::Min(1000,$distance*400)} else {0}
                if($forward) {$all=$false}
                Command $role $forward $yaw
            }
            Sample "waypoint-$point"
            if($all -and !$overrideText) {$reached=$true;break}
            Start-Sleep -Milliseconds 250
        }
        foreach($role in @('host','client')) { $state=Read-State $role; Command $role 0 ([int]([Math]::Atan2([Math]::Sin($state.yaw),[Math]::Cos($state.yaw))*1000)) }
        if(!$reached) { throw "Waypoint $point ($($goal -join ',')) blocked; see trace and both windows." }
        Write-Output "Reached waypoint $point ($($goal -join ',')) on both peers."
    }
    $state=Read-State 'client'
    $dx=71.478-$state.x; $dz=-9.2099-$state.z
    $yaw=[int]([Math]::Atan2(-$dx,-$dz)*1000)
    Command 'client' 0 $yaw 0
    Start-Sleep -Seconds 1
    $state=Read-State 'client'
    if($state.focus -ne 'level_wood_1') { throw "Original exit is not focused: $($state.focus)." }
    Command 'client' 0 $yaw 0 1
    Start-Sleep -Milliseconds 400
    Command 'client' 0 $yaw 0 0
    $deadline=[DateTime]::UtcNow.AddSeconds(40);$completed=$false
    while([DateTime]::UtcNow -lt $deadline) {
        Sample 'original-exit'
        $h=Read-State 'host';$c=Read-State 'client'
        if($h -and $c -and $h.map -match '01_old_archives' -and $c.map -match '01_old_archives' -and $h.epoch -eq $c.epoch -and $h.active -and $c.active) {$completed=$true;break}
        Start-Sleep -Milliseconds 250
    }
    if(!$completed) {throw 'Original exit interaction did not deliver active players to Old Archives on both peers.'}
    [pscustomobject]@{completed=$true;method='normal engine inputs with original campaign scripts and physics';host=$h;client=$c;finishedUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'result.json')
    Write-Output 'PASS: original Rainy Hall route and client exit interaction delivered both peers to Old Archives.'
} catch {
    [pscustomobject]@{completed=$false;status='failed';reason=$_.Exception.Message;finishedUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'result.json')
    throw
} finally {
    foreach($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name]) }
    if($KeepOpen -and !$Resume) {
        Write-Output 'Windows retained for screenshots; set build/level-playthrough/finish.flag to done to finish.'
        while(@($processes | Where-Object {!$_.HasExited}).Count -gt 0 -and [IO.File]::ReadAllText((Join-Path $run 'finish.flag')).Trim() -ne 'done') {Start-Sleep -Milliseconds 250}
    }
    foreach($role in @('host','client')) {
        $metaPath=Join-Path $run "$role-process.json"
        if(Test-Path $metaPath) {$meta=Get-Content $metaPath | ConvertFrom-Json;if(Test-Path -LiteralPath $meta.log) {Copy-Item -LiteralPath $meta.log -Destination (Join-Path $run "$role.log")}}
    }
    if(!$KeepOpen) {foreach($p in $processes) {if(!$p.HasExited) {Stop-Process -Id $p.Id}}}
}
