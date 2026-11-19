# SPDX-License-Identifier: GPL-3.0-or-later
# Invito Steam e rientro in una partita in corso, su un solo PC.
# Due copie dello stesso account Steam non possono invitarsi, quindi Steam è
# sostituito da AMNESIA_NET_TEST_STEAM_FAKE (porte UDP locali): lobby, invito,
# ingresso dalla richiesta di Steam e ponte dei messaggi sono il codice vero.
# -Rejoin: il client va in crash a partita avviata, si riavvia, accetta di nuovo
# l'invito e rientra nella partita che l'host ha tenuto in pausa.
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [int]$Port = 27070,
    [string]$StartMap = '02_entrance_hall.map',
    [switch]$Rejoin,
    [switch]$Screenshot,
    [switch]$Visual,
    [string]$Executable = "$PSScriptRoot/../../amnesia/src/game/x64/Release/Lux.exe"
)
$ErrorActionPreference = 'Stop'
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$exe = (Resolve-Path -LiteralPath $Executable).Path
$run = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path 'build/steam-invite'
New-Item -ItemType Directory -Force -Path $run | Out-Null
$id = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$fakeHost = $Port + 100; $fakeClient = $Port + 101
$names = @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_ADDRESS','AMNESIA_NET_SMOKE_SECONDS','AMNESIA_NET_TEST_STEAM_FAKE','AMNESIA_NET_TEST_STEAM_JOIN','AMNESIA_NET_TEST_STEAM_FRIEND','AMNESIA_STEAM_PRESENCE','AMNESIA_QA_GAME_SHOT')
$saved = @{}; foreach($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
$logs = @{}; $inits = @{}; $processes = @{}
$baseConfig = [IO.File]::ReadAllText((Join-Path $game 'config/main_init.cfg'))
function Start-Role([string]$role, [int]$seconds) {
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE', $role)
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT', [string]$Port)
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS', '127.0.0.1')
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS', [string]$seconds)
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_STEAM_FAKE', [string]$(if($role -eq 'host') { $fakeHost } else { $fakeClient }))
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_STEAM_JOIN', $(if($role -eq 'client') { [string]$fakeHost } else { $null }))
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_STEAM_FRIEND', $(if($role -eq 'host') { [string]$fakeClient } else { $null }))
    [Environment]::SetEnvironmentVariable('AMNESIA_STEAM_PRESENCE', $null)
    # Stessa inquadratura da host e client per confrontare la qualità dell'immagine.
    [Environment]::SetEnvironmentVariable('AMNESIA_QA_GAME_SHOT', $(if($Screenshot) { Join-Path $run "$role.png" } else { $null }))
    $style = if($Visual) { 'Normal' } else { 'Hidden' }
    return Start-Process -FilePath $exe -WorkingDirectory $game -ArgumentList $inits[$role] -WindowStyle $style -PassThru
}
function Read-Log([string]$path) {
    # Il gioco tiene aperto hpl.log: lettura condivisa.
    $stream = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try { return (New-Object IO.StreamReader($stream)).ReadToEnd() } finally { $stream.Dispose() }
}
function Wait-Log([string]$role, [string]$marker, [int]$seconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    while([DateTime]::UtcNow -lt $deadline) {
        if((Test-Path $logs[$role]) -and (Read-Log $logs[$role]).Contains($marker)) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
try {
    foreach($role in @('host','client')) {
        $profile = "OnlineCoopSteam_${id}_$role"
        $logs[$role] = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
        $settings = Join-Path $run "$role-settings.cfg"
        $show = if($role -eq 'host') { 'false' } else { 'true' }
        $width = if($Screenshot) { 1280 } else { 640 }; $height = if($Screenshot) { 720 } else { 480 }
        # SaveConfig: il client conserva la propria identità di ripresa tra un avvio e l'altro.
        @"
<Main ShowMenu="$show" ShowPreMenu="false" SaveConfig="true" DefaultProfileName="net_test" UpdateLogActive="false" />
<Graphics TextureQuality="2" Shadows="0" SSAOActive="false" />
<Engine LimitFPS="true" />
<Screen Width="$width" Height="$height" Display="0" FullScreen="false" Vsync="false" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
<Online AutoProfile="true" />
"@ | Set-Content -LiteralPath $settings -Encoding utf8
        $init = $baseConfig -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settings`""
        if($role -eq 'host') { $init = $init -replace 'File\s*=\s*"00_rainy_hall.map"', "File = `"$StartMap`"" }
        $inits[$role] = Join-Path $run "$role-init.cfg"
        $init | Set-Content -LiteralPath $inits[$role] -Encoding utf8
    }
    $processes['host'] = Start-Role 'host' $(if($Rejoin) { 150 } else { 70 })
    Start-Sleep -Seconds 3
    $processes['client'] = Start-Role 'client' $(if($Rejoin) { 200 } else { 60 })
    if(!(Wait-Log 'host' 'Steam lobby and relay bridge started' 60)) { throw 'Host did not open the Steam lobby and bridge' }
    if(!(Wait-Log 'client' 'Connecting to the host through Steam' 60)) { throw 'Client did not join through the Steam invite' }
    if(!(Wait-Log 'client' 'Complete host entity baseline applied' 90)) { throw 'Client did not receive the world through the Steam bridge' }
    Write-Output 'PASS: invite accepted, lobby joined and world received through the Steam message bridge'
    if($Screenshot) {
        foreach($role in @('host','client')) { if(!(Wait-Log $role 'index=1' 60)) { throw "$role did not save the game screenshot" } }
        Write-Output "PASS: host and client screenshots in $run"
    }
    if($Rejoin) {
        Start-Sleep -Seconds 5
        Copy-Item $logs['client'] (Join-Path $run 'client-first.log') -Force
        Stop-Process -Id $processes['client'].Id -Force   # crash del client a partita avviata
        if(!(Wait-Log 'host' 'Connection lost; campaign paused for recovery' 30)) { throw 'Host did not pause the campaign after the client crash' }
        Write-Output 'PASS: host paused the running campaign after the client crash'
        Start-Sleep -Seconds 2
        $processes['client'] = Start-Role 'client' 60
        if(!(Wait-Log 'client' 'Connecting to the host through Steam' 60)) { throw 'Restarted client did not accept the invite again' }
        if(!(Wait-Log 'host' 'Partner reconnected; rebuilding authoritative baseline' 60)) { throw 'Host did not accept the returning partner' }
        if(!(Wait-Log 'client' 'Complete host entity baseline applied' 90)) { throw 'Returning client did not receive the running world' }
        Write-Output 'PASS: restarted client rejoined the running campaign through Steam'
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(240)
    while(@($processes.Values | Where-Object { !$_.HasExited }).Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 250 }
    foreach($role in @('host','client')) {
        if(!$processes[$role].HasExited) { throw "$role did not exit" }
        Copy-Item $logs[$role] (Join-Path $run "$role.log") -Force
        $log = Read-Log $logs[$role]
        if(!$log.Contains('Smoke test finished')) { throw "$role did not finish cleanly: $($logs[$role])" }
        if($log.Contains('different resume identity')) { throw "$role rejected the returning identity" }
    }
    Write-Output "PASS: Steam invite$(if($Rejoin){' and rejoin'}) with clean exits; logs $run"
} finally {
    foreach($p in $processes.Values) { if($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force } }
    foreach($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
