# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [int]$Port = 27068,
    [string]$Executable = "$PSScriptRoot/../../amnesia/src/game/x64/Release/Lux.exe"
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$exe = (Resolve-Path -LiteralPath $Executable).Path
$run = Join-Path $repo ('build/movement-stop/' + [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$processes = @(); $logs = @{}; $saved = @{}; $serial = 1
$culture = [Globalization.CultureInfo]::InvariantCulture
foreach($name in [Environment]::GetEnvironmentVariables().Keys) {
    if($name.StartsWith('AMNESIA_NET_')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
}
function Read-Numbers([string]$name, [int]$count) {
    $path = Join-Path $run $name
    if(!(Test-Path -LiteralPath $path) -or ([DateTime]::UtcNow - (Get-Item $path).LastWriteTimeUtc).TotalSeconds -gt 2) { return $null }
    try { $fields = [IO.File]::ReadAllText($path).Trim() -split '\s+' } catch [IO.IOException] { return $null }
    if($fields.Count -ne $count) { return $null }
    $values = @()
    foreach($field in $fields) { $values += [float]::Parse($field,$culture) }
    return ,$values
}
function Command([int]$forward,[int]$strafe) {
    $script:serial++
    for($attempt=0;$attempt -lt 10;$attempt++) {
        try { [IO.File]::WriteAllText((Join-Path $run 'client.drive'), "$script:serial 120 $forward $strafe 0 0 0 0"); return }
        catch [IO.IOException] { if($attempt -eq 9) { throw }; Start-Sleep -Milliseconds 10 }
    }
}
function Read-LiveLog([string]$path) {
    if(!(Test-Path $path)) { return '' }
    try {
        $stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
        $reader = [IO.StreamReader]::new($stream)
        try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
    } catch [IO.IOException] { return '' }
}
function Client-Position {
    $path = Join-Path $run 'client.drive.state'
    if(!(Test-Path $path) -or ([DateTime]::UtcNow - (Get-Item $path).LastWriteTimeUtc).TotalSeconds -gt 2) { return $null }
    try { $fields = [IO.File]::ReadAllText($path).Trim() -split '\s+' } catch [IO.IOException] { return $null }
    if($fields.Count -ne 10) { return $null }
    return ,@([float]::Parse($fields[2],$culture),[float]::Parse($fields[4],$culture))
}
try {
    foreach($name in @($saved.Keys)) { [Environment]::SetEnvironmentVariable($name,$null) }
    $tag = Split-Path $run -Leaf
    foreach($role in @('host','client')) {
        $profile = "OnlineCoopStop_${tag}_$role"
        $logs[$role] = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
        $settings = Join-Path $run "$role-settings.cfg"
        $menu = if($role -eq 'host') {'false'} else {'true'}
        @"
<Main ShowMenu="$menu" ShowPreMenu="false" SaveConfig="false" DefaultProfileName="net_test" UpdateLogActive="false" />
<Graphics TextureQuality="2" Shadows="0" SSAOActive="false" />
<Engine LimitFPS="true" />
<Screen Width="640" Height="480" Display="0" FullScreen="false" Vsync="false" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
"@ | Set-Content -LiteralPath $settings -Encoding utf8
        $init = [IO.File]::ReadAllText((Join-Path $game 'config/main_init.cfg'))
        $init = $init -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settings`""
        $init = $init -replace 'File\s*=\s*"00_rainy_hall.map"', 'File = "02_entrance_hall.map"'
        $config = Join-Path $run "$role-init.cfg"; [IO.File]::WriteAllText($config,$init)
        $drive = Join-Path $run "$role.drive"; [IO.File]::WriteAllText($drive,'1 120 0 0 0 0 0 0')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE',$role)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT',[string]$Port)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS','127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS','45')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_DRIVE_FILE',$drive)
        $processes += Start-Process -FilePath $exe -WorkingDirectory $game -ArgumentList $config -WindowStyle Hidden -PassThru
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while([DateTime]::UtcNow -lt $deadline) {
        if((Read-Numbers 'host.drive.peer.motion' 2) -and (Client-Position) -and
            (Read-LiveLog $logs.client).Contains('Complete host entity baseline applied')) { break }
        Start-Sleep -Milliseconds 100
    }
    if([DateTime]::UtcNow -ge $deadline) { throw 'Client baseline/QA telemetry not ready' }
    $results = @()
    foreach($axis in @(0,1)) {
        Command $(if($axis -eq 0) {1000} else {0}) $(if($axis -eq 1) {1000} else {0})
        $deadline = [DateTime]::UtcNow.AddSeconds(2)
        $peak = 0.0
        while([DateTime]::UtcNow -lt $deadline) {
            $motion = Read-Numbers 'host.drive.peer.motion' 2
            if($motion) { $peak = [Math]::Max($peak,[Math]::Abs($motion[$axis])) }
            if($peak -gt 0.15) { break }
            Start-Sleep -Milliseconds 50
        }
        if($peak -le 0.15) { throw "Axis $axis never accelerated: stop assertion would be inconclusive" }
        Command 0 0
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        while([DateTime]::UtcNow -lt $deadline) {
            $motion = Read-Numbers 'host.drive.peer.motion' 2
            if($motion -and [Math]::Abs($motion[0]) -lt 0.01 -and [Math]::Abs($motion[1]) -lt 0.01) { break }
            Start-Sleep -Milliseconds 50
        }
        if([DateTime]::UtcNow -ge $deadline) { throw "Axis $axis kept moving after input release: $motion" }
        # Let the last snapshot and pending client prediction settle.
        Start-Sleep -Milliseconds 750
        $hostBefore = Read-Numbers 'host.drive.peer' 7; $clientBefore = Client-Position
        $worstSpeed = 0.0
        for($sample=0;$sample -lt 10;$sample++) {
            Start-Sleep -Milliseconds 100
            $motion = Read-Numbers 'host.drive.peer.motion' 2
            if(!$motion) { throw 'Missing live movement telemetry' }
            $worstSpeed = [Math]::Max($worstSpeed,[Math]::Max([Math]::Abs($motion[0]),[Math]::Abs($motion[1])))
        }
        $hostAfter = Read-Numbers 'host.drive.peer' 7; $clientAfter = Client-Position
        if(!$hostBefore -or !$hostAfter -or !$clientBefore -or !$clientAfter) { throw 'Missing live position telemetry' }
        $hostDrift = [Math]::Sqrt([Math]::Pow($hostAfter[0]-$hostBefore[0],2)+[Math]::Pow($hostAfter[2]-$hostBefore[2],2))
        $clientDrift = [Math]::Sqrt([Math]::Pow($clientAfter[0]-$clientBefore[0],2)+[Math]::Pow($clientAfter[1]-$clientBefore[1],2))
        if($worstSpeed -gt 0.01 -or $hostDrift -gt 0.04 -or $clientDrift -gt 0.04) { throw "Axis $axis idle drift: speed=$worstSpeed host=$hostDrift client=$clientDrift" }
        $results += [pscustomobject]@{axis=$axis;movingSpeed=$peak;idleSpeed=$worstSpeed;hostDriftMetres=$hostDrift;clientDriftMetres=$clientDrift}
    }
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'results.json')
    $deadline = [DateTime]::UtcNow.AddSeconds(50)
    while(@($processes | Where-Object {!$_.HasExited}).Count -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    foreach($process in $processes) { $process.Refresh(); if(!$process.HasExited -or $process.ExitCode -ne 0) { throw 'Movement test process failed or timed out' } }
    foreach($role in @('host','client')) {
        $log = [IO.File]::ReadAllText($logs[$role])
        if(!$log.Contains('Smoke test finished') -or $log.Contains('Rejected:') -or $log.Contains('[NET/TEST] FAIL:')) { throw "$role movement test failed" }
    }
    Write-Output "PASS: forward/strafe release stops authoritative body and client prediction; evidence $run"
} finally {
    foreach($process in $processes) { if(!$process.HasExited) { Stop-Process -Id $process.Id; [void]$process.WaitForExit(3000) } }
    foreach($role in @('host','client')) { if($logs.ContainsKey($role) -and (Test-Path $logs[$role])) { Copy-Item -LiteralPath $logs[$role] -Destination (Join-Path $run "$role.log") -Force } }
    foreach($name in @([Environment]::GetEnvironmentVariables().Keys)) { if($name.StartsWith('AMNESIA_NET_')) { [Environment]::SetEnvironmentVariable($name,$null) } }
    foreach($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name]) }
}
