# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [ValidateSet('reconnect','migrate')][string]$Mode = 'reconnect',
    [int]$Port = 27060,
    [switch]$SlowCheckpoint,
    [switch]$StaleCheckpoint,
    [switch]$SerializationParity,
    [switch]$CancelCapture,
    [string]$Executable = "$PSScriptRoot/../../amnesia/src/game/x64/Release/Lux.exe"
)
$ErrorActionPreference = 'Stop'
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$exe = (Resolve-Path -LiteralPath $Executable).Path
$run = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path "build/recovery-qa/$Mode"
New-Item -ItemType Directory -Force -Path $run | Out-Null
$id = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$processes = @(); $logs = @{}
$saved = @{}
foreach($name in [Environment]::GetEnvironmentVariables().Keys) { if($name.StartsWith('AMNESIA_NET_')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) } }
try {
    foreach($name in @($saved.Keys)) { [Environment]::SetEnvironmentVariable($name,$null) }
    foreach($role in @('host','client')) {
        $profile = "OnlineCoopRecovery_${id}_$role"
        $logs[$role] = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
        $settings = Join-Path $run "$role-settings.cfg"
        $show = if($role -eq 'host') {'false'} else {'true'}
        @"
<Main ShowMenu="$show" ShowPreMenu="false" SaveConfig="false" DefaultProfileName="net_test" UpdateLogActive="false" />
<Graphics TextureQuality="2" Shadows="0" SSAOActive="false" />
<Engine LimitFPS="true" />
<Screen Width="640" Height="480" Display="0" FullScreen="false" Vsync="false" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
"@ | Set-Content -LiteralPath $settings -Encoding utf8
        $init = [IO.File]::ReadAllText((Join-Path $game 'config/main_init.cfg'))
        $init = $init -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settings`""
        $init = $init -replace 'File\s*=\s*"00_rainy_hall.map"', 'File = "02_entrance_hall.map"'
        $path = Join-Path $run "$role-init.cfg"; [IO.File]::WriteAllText($path,$init)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE',$role)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT',[string]$Port)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS','127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS','60')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_RECOVERY',$Mode)
        if($SlowCheckpoint) { [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_SLOW_CHECKPOINT','1') }
        if($StaleCheckpoint) { [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_STALE_CHECKPOINT','1') }
        if($SerializationParity) { [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_SERIALIZATION_PARITY','1') }
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_CANCEL_CAPTURE',$(if($CancelCapture -and $role -eq 'host') {'1'} else {$null}))
        $processes += Start-Process -FilePath $exe -WorkingDirectory $game -ArgumentList $path -WindowStyle Hidden -PassThru
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(95)
    while(@($processes | Where-Object {!$_.HasExited}).Count -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 250 }
    foreach($process in $processes) { $process.Refresh(); if(!$process.HasExited -or $process.ExitCode -ne 0) { throw 'Recovery game process failed or timed out' } }
    foreach($role in @('host','client')) {
        $text = [IO.File]::ReadAllText($logs[$role]); [IO.File]::WriteAllText((Join-Path $run "$role.log"),$text)
        if($text.Contains('[NET/TEST] FAIL:') -or $text.Contains('Invalid native save schema') -or $text.Contains('Promotion failed')) { throw "$role recovery state validation failed" }
        foreach($marker in @('Smoke test finished','Partner reconnected; rebuilding authoritative baseline','Journal history verified: one note, two ordered diaries, active and completed quests')) {
            if(!$text.Contains($marker)) { throw "Missing $role recovery evidence: $marker" }
        }
        if(([regex]::Matches($text,'Protocol handshake accepted')).Count -lt 2) { throw "$role did not reconnect" }
    }
    $client = [IO.File]::ReadAllText((Join-Path $run 'client.log')); $hostText = [IO.File]::ReadAllText((Join-Path $run 'host.log'))
    if(!$client.Contains('Complete campaign checkpoint received')) { throw 'Client never received validated campaign checkpoint' }
    if(!$hostText.Contains('Missing particle save clears stale/non-finite fields')) { throw 'Missing particle save regression did not run' }
    if($SerializationParity -and !$hostText.Contains('Sliced checkpoint XML exactly matches native save serialization')) { throw 'Incremental serialization parity did not pass' }
    if($SerializationParity -and !$hostText.Contains('Concurrent numeric and wide-string save formatting verified')) { throw 'Concurrent native save formatting did not pass' }
    if($CancelCapture -and (!$hostText.Contains('Historical maps reset during sliced checkpoint capture') -or !$hostText.Contains('Obsolete sliced checkpoint capture discarded'))) { throw 'Capture revision guard did not discard a stale historical-map iterator' }
    if($StaleCheckpoint) {
        $invalidated = $client.IndexOf('Invalidated in-flight checkpoint context')
        $discarded = $client.IndexOf('Obsolete checkpoint worker result discarded')
        $accepted = $client.IndexOf('Complete campaign checkpoint received')
        if($invalidated -lt 0 -or $discarded -le $invalidated -or $accepted -le $discarded) { throw 'Stale worker was not discarded before accepting a later valid checkpoint' }
        Write-Output 'PASS: stale in-flight worker discarded; later validated checkpoint restored recovery'
    }
    if($SlowCheckpoint) {
        $timings = [regex]::Matches($client,'worker_validation_ms=(\d+) main_max_us=(\d+)')
        $responsive = @($timings | Where-Object { [int]$_.Groups[1].Value -ge 350 -and [int]$_.Groups[2].Value -lt 16000 })
        if(!$responsive.Count) { throw 'Slow worker did not complete without a 16 ms game-thread checkpoint stall' }
        Write-Output 'PASS: deliberately delayed checkpoint worker (350 ms) completed with game-thread slices below 16 ms'
    }
    if($Mode -eq 'migrate') {
        if(!$client.Contains('Host migrated from complete campaign checkpoint') -or !$client.Contains('Migration restored campaign variable and swapped individual player states') -or !$hostText.Contains('Former host yielded authority')) { throw 'Host migration did not restore campaign and reverse roles' }
    } else { if(!$hostText.Contains('Original host listening again after two-second outage') -or !$client.Contains('Replica loaded epoch=2')) { throw 'Short outage recovery did not rebuild client map' } }
    Write-Output "PASS: $Mode real two-process campaign recovery, journal chronology and clean exits; logs $run"
} finally {
    foreach($role in @('host','client')) { if($logs.ContainsKey($role) -and (Test-Path -LiteralPath $logs[$role])) { Copy-Item -LiteralPath $logs[$role] -Destination (Join-Path $run "$role.log") -Force } }
    foreach($process in $processes) { if(!$process.HasExited) { Stop-Process -Id $process.Id } }
    foreach($name in @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_ADDRESS','AMNESIA_NET_SMOKE_SECONDS','AMNESIA_NET_TEST_RECOVERY','AMNESIA_NET_TEST_SLOW_CHECKPOINT','AMNESIA_NET_TEST_STALE_CHECKPOINT','AMNESIA_NET_TEST_SERIALIZATION_PARITY','AMNESIA_NET_TEST_CANCEL_CAPTURE')) { [Environment]::SetEnvironmentVariable($name,$null) }
    foreach($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name]) }
}
