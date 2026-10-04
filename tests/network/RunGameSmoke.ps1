param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [string]$Executable = "$PSScriptRoot/../../amnesia/src/game/x64/Release/Lux.exe",
    [int]$Port = 27032,
    [int]$ClientPort = 0,
    [switch]$Replication,
    [switch]$Campaign,
    [switch]$Media,
    [switch]$Prediction,
    [switch]$SnapshotBlackout,
    [switch]$OnlineMenu,
    [switch]$VisibilityProps,
    [ValidatePattern('^[a-zA-Z0-9_-]+\.map$')][string]$StartMap = '02_entrance_hall.map',
    [switch]$Visual,
    [switch]$MapOnly,
    [switch]$Enemy
)
$ErrorActionPreference = 'Stop'
if ($Media) { $Campaign = $true }
if ($Campaign) { $Replication = $true }
if ($Enemy) { $MapOnly = $true }
if ($OnlineMenu) { $MapOnly = $true }
if ($VisibilityProps) { $MapOnly = $true }
$gamePath = (Resolve-Path -LiteralPath $GameDirectory).Path
$exePath = (Resolve-Path -LiteralPath $Executable).Path
$repoPath = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$runPath = Join-Path $repoPath $(if ($OnlineMenu) { 'build/online-menu' } elseif ($Enemy) { 'build/enemy-test' } elseif ($MapOnly) { "build/visual-qa/$($StartMap.Replace('.map',''))" } elseif ($Replication) { 'build/player-replication' } else { 'build/game-smoke' })
New-Item -ItemType Directory -Force -Path $runPath | Out-Null
$runId = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$settingsPath = Join-Path $runPath 'settings.cfg'
@'
<Main ShowMenu="true" ShowPreMenu="false" SaveConfig="false" DefaultProfileName="net_test" UpdateLogActive="false" />
<Graphics TextureQuality="2" Shadows="0" SSAOActive="false" />
<Engine LimitFPS="true" />
<Screen Width="640" Height="480" Display="0" FullScreen="false" Vsync="false" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
'@ | Set-Content -LiteralPath $settingsPath -Encoding utf8
$baseConfig = [IO.File]::ReadAllText((Join-Path $gamePath 'config/main_init.cfg'))
$profiles = @{}
$processes = @()
$environmentNames = @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_ADDRESS','AMNESIA_NET_SMOKE_SECONDS','AMNESIA_NET_TEST_MOVEMENT','AMNESIA_NET_TEST_PICKUP','AMNESIA_NET_TEST_CAMPAIGN','AMNESIA_NET_TEST_ENEMY','AMNESIA_NET_TEST_MEDIA','AMNESIA_NET_TEST_DRIVE_FILE','AMNESIA_NET_TEST_RECOVERY','AMNESIA_NET_TEST_PREDICTION','AMNESIA_NET_TEST_SNAPSHOT_BLACKOUT','AMNESIA_NET_TEST_MENU','AMNESIA_NET_TEST_VISIBILITY_PROPS')
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    foreach ($role in @('host','client')) {
        $profile = "OnlineCoopSmoke_${runId}_$role"
        $profiles[$role] = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
        $init = $baseConfig -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        if ($Visual) { $init = $init -replace 'GameName\s*=\s*"[^"]*"', "GameName = `"Amnesia Online QA [$role]`"" }
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settingsPath`""
        if (($Replication -or $MapOnly) -and $role -eq 'host') {
            $hostSettings = Join-Path $runPath 'host-settings.cfg'
            ([IO.File]::ReadAllText($settingsPath) -replace 'ShowMenu="true"', 'ShowMenu="false"') | Set-Content -LiteralPath $hostSettings -Encoding utf8
            $init = $init.Replace($settingsPath, $hostSettings)
            $init = $init -replace 'File\s*=\s*"00_rainy_hall.map"', "File = `"$StartMap`""
        }
        $initPath = Join-Path $runPath "$role-init.cfg"
        $init | Set-Content -LiteralPath $initPath -Encoding utf8
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE', $role)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_VISIBILITY_PROPS', $(if ($VisibilityProps) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_MENU', $(if ($OnlineMenu) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_DRIVE_FILE', $null)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT', [string]$(if ($role -eq 'client' -and $ClientPort -gt 0) { $ClientPort } else { $Port }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS', '127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS', $(if ($Campaign -or $MapOnly) { '60' } elseif ($Replication) { '30' } else { '15' }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_MOVEMENT', $(if ($Replication -and $role -eq 'client') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_PICKUP', $(if ($Replication -and $role -eq 'host') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_CAMPAIGN', $(if ($Campaign -and $role -eq 'host') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_ENEMY', $(if ($Enemy) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_MEDIA', $(if ($Media) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_RECOVERY', $null)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_SNAPSHOT_BLACKOUT', $(if ($SnapshotBlackout) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_PREDICTION', $(if ($Prediction -and $role -eq 'client') { '1' } else { $null }))
        $style = if ($Visual) { 'Normal' } else { 'Hidden' }
        $processes += Start-Process -FilePath $exePath -WorkingDirectory $gamePath -ArgumentList $initPath -WindowStyle $style -PassThru
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($(if ($Campaign -or $MapOnly) { 90 } else { 50 }))
    while (@($processes | Where-Object { !$_.HasExited }).Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    foreach ($p in $processes) {
        if (!$p.HasExited) { throw 'Game smoke timeout; inspect runtime logs' }
        $p.Refresh()
        if ($p.ExitCode -ne 0) { throw "Game process exited with $($p.ExitCode)" }
    }
    foreach ($role in @('host','client')) {
        $log = [IO.File]::ReadAllText($profiles[$role])
        [IO.File]::WriteAllText((Join-Path $runPath "$role.log"), $log)
    }
    foreach ($role in @('host','client')) {
        $log = [IO.File]::ReadAllText((Join-Path $runPath "$role.log"))
        if (!$log.Contains('Protocol handshake accepted') -or !$log.Contains('Smoke test finished')) {
            throw "$role did not finish a handshake and clean smoke exit: $($profiles[$role])"
        }
        Write-Output "PASS: $role game handshake and clean exit ($($profiles[$role]))"
        if($OnlineMenu) {
            foreach($marker in @('Clear image checkbox suppresses and restores original blur without changing graphics preferences','Online menu exit/reconnect regression completed')) {
                if(!$log.Contains($marker)) { throw "Missing $role online menu proof: $marker" }
            }
            if($log.Contains('Online menu FAILED:')) { throw "$role online menu failed" }
            if(([regex]::Matches($log,'Protocol handshake accepted')).Count -lt 3) { throw "$role did not complete both reconnects" }
            if($role -eq 'client') {
                foreach($marker in @('Exit stays at main menu with transport stopped','Save and exit stays at main menu with transport stopped')) {
                    if(!$log.Contains($marker)) { throw "Missing client exit proof: $marker" }
                }
            }
            Write-Output "PASS: $role clear image, deliberate exits and reconnect button"
        }
        if($SnapshotBlackout) {
            $proof = if($role -eq 'host') { 'Ordinary snapshots suppressed for three seconds' } else { 'Reliable snapshot backups applied without life reset' }
            if(!$log.Contains($proof)) { throw "Missing $role snapshot blackout proof: $proof" }
            if($role -eq 'client') {
                # The blackout fixture runs before the first save/reload. Later
                # campaign transitions synchronously load maps on the host;
                # report those pauses separately from the backup regression.
                $initialMap = [regex]::Match($log,'Replica loaded epoch=(\d+)')
                if(!$initialMap.Success) { throw 'Snapshot blackout did not load its initial map' }
                foreach($gap in [regex]::Matches($log,'Authoritative snapshot gap=(\d+) ms epoch=(\d+)')) {
                    if($gap.Groups[2].Value -eq $initialMap.Groups[1].Value) {
                        if([int]$gap.Groups[1].Value -gt 700) { throw 'Snapshot backup allowed a gap above 700 ms in the blackout epoch' }
                    } else {
                        Write-Output "INFO: later campaign epoch $($gap.Groups[2].Value) snapshot pause $($gap.Groups[1].Value) ms; inspect host loading log"
                    }
                }
            }
            Write-Output "PASS: $role ordinary snapshot blackout recovery"
        }
        if ($Media) {
            if($role -eq 'host' -and !$log.Contains('Repeated particle destruction safely ignores stale owner pointer')) { throw 'Stale particle owner pointer regression did not pass' }
            $mediaMarkers = if ($role -eq 'host') {
                @('Effect reset clears non-finite checkpoint fade speeds', 'Saved sound/particle and music fixtures created on host', 'Host save/load retained sound and particle',
                  'Media properties changed on host', 'Media fixtures removed on host', 'Host validated remote lantern with independent oil')
            } else {
                @('Media created on client kind=1', 'Media created on client kind=2',
                  'Saved media baseline restored on client kind=1', 'Saved media baseline restored on client kind=2',
                  'Media property delta applied on client kind=1', 'Media property delta applied on client kind=2',
                  'Media removal applied on client kind=1', 'Media removal applied on client kind=2',
                  'Music state applied on client', 'Music stop applied on client', 'GUI sound event applied on client',
                  'Global sound volume fade applied on client', 'Global sound speed fade applied on client', 'Screen shake applied on client',
                  'Remote host lantern emits light on client', 'Both lanterns emit light with independent oil on client',
                  'Independent lantern toggles applied on client', 'Injected stale local lantern permissions on client',
                  'Client held lantern follows moving camera with stale local permissions',
                  'Lantern save/load baseline and repeated snapshots stayed silent',
                  'Client lantern on/off/on produced exactly one audible event per toggle',
                  'Lantern audio one-shot counts verified on=2 off=3 including depletion and empty attempt')
            }
            foreach ($marker in $mediaMarkers) { if (!$log.Contains($marker)) { throw "Missing $role media proof: $marker" } }
            if ($log.Contains('FAILED') -or $log.Contains('closing session')) { throw "$role media integration failed" }
            if($role -eq 'client') {
                $audioEvents = [regex]::Matches($log, 'Owner lantern audio channel playing=1 file=ui_lantern_(on|off) target=2')
                if($audioEvents.Count -ne 5) { throw "Expected exactly five audible owner lantern events; got $($audioEvents.Count)" }
            } elseif(!$log.Contains('Requested authoritative remote lantern oil depletion')) {
                throw 'Missing host lantern depletion fixture'
            }
            Write-Output "PASS: $role sound/particle lifecycle, music, effects, saved media, independent lanterns and owner audio"
        }
        if ($MapOnly) {
            if($VisibilityProps) {
                $visibilityMarkers = @('Full starting oil verified on local and remote player','Original eye adaptation emits local ambient light in darkness')
                if($role -eq 'host') { $visibilityMarkers += 'Authoritative box and door motion fixtures created' }
                else { $visibilityMarkers += @('Prop rendered between authoritative poses: online_test_motion_box','Prop rendered between authoritative poses: online_test_motion_door','Settled authoritative prop pose verified: online_test_motion_box','Settled authoritative prop pose verified: online_test_motion_door') }
                foreach($marker in $visibilityMarkers) { if(!$log.Contains($marker)) { throw "Missing $role visibility/prop proof: $marker" } }
                if($log.Contains('FAILED')) { throw "$role visibility/prop check failed" }
                Write-Output "PASS: $role starting oil, darkness adaptation and authoritative moving props"
            }
            if($role -eq 'client' -and (!$log.Contains('Complete campaign checkpoint received') -or $log.Contains('Invalid native save schema'))) {
                throw 'Map replica did not receive valid native campaign checkpoints'
            }
            if ($log.Contains('Spawned remote pickup fixture') -or $log.Contains('Requested authoritative campaign level transition')) { throw 'Map-only run unexpectedly enabled scripted test fixtures' }
            $markers = if ($role -eq 'host') { @('Complete host entity baseline sent') } else { @('Complete host entity baseline applied','First authoritative player snapshot applied') }
            foreach ($marker in $markers) { if (!$log.Contains($marker)) { throw "Missing $role map proof: $marker" } }
            if ($log.Contains('Rejected:') -or $log.Contains('closing session') -or $log.Contains('readiness timed out')) { throw "$role map synchronization failed" }
            Write-Output "PASS: $role baseline and player replication on $StartMap"
            if($Enemy) {
                $enemyMarkers = if($role -eq 'host') { @('Spawned authoritative enemy fixture','Enemy occlusion and switching between both players verified','Host enemy moved under authoritative AI','Host enemy caused player damage') } else { @('Authoritative enemy created on client','Authoritative enemy movement applied on client','Host enemy damage applied on client') }
                foreach($marker in $enemyMarkers) { if(!$log.Contains($marker)) { throw "Missing $role enemy proof: $marker" } }
                Write-Output "PASS: $role enemy motion and damage replication"
            }
        }
        if ($Replication) {
            if($Prediction -and $role -eq 'client' -and !$log.Contains('Local movement prediction applied before host acknowledgement with world collision checks')) { throw 'Native prediction did not exercise pending input and collision checks' }
            if($Prediction -and $role -eq 'client' -and !$log.Contains('Small authoritative movement correction smoothed with collision checks')) { throw 'Native movement smoothing was not exercised' }
            $markers = if ($role -eq 'host') { @('Announced epoch=', 'Remote Player 2 moved under network input', 'Complete host entity baseline sent', 'Host validated remote pickup; exactly one tinderbox awarded') } else { @('Replica loaded epoch=', 'First authoritative player snapshot applied', 'Created host avatar in replica world', 'Complete host entity baseline applied', 'Client camera/body moved from authoritative snapshots', 'Host pickup destruction replicated to client') }
            foreach ($marker in $markers) { if (!$log.Contains($marker)) { throw "Missing $role replication proof: $marker" } }
            $inventoryMarker = if ($role -eq 'host') { 'Host validated remote inventory use; one potion consumed' } else { 'Shared host inventory applied' }
            if (!$log.Contains($inventoryMarker)) { throw "Missing $role inventory proof: $inventoryMarker" }
            if ($role -eq 'client' -and (!$log.Contains('Host scripted lighting applied') -or !$log.Contains('Host story message applied on client'))) { throw 'Client story/light presentation failed' }
            if ($Campaign) {
                if($role -eq 'client' -and (!$log.Contains('Complete campaign checkpoint received') -or $log.Contains('Invalid native save schema'))) {
                    throw 'Client did not retain valid native campaign checkpoints throughout the integration test'
                }
                if (!$log.Contains('epoch=3') -or !$log.Contains('01_old_archives.map')) { throw "$role failed campaign map/save synchronization" }
                if ($role -eq 'host' -and !$log.Contains('Host save/load retained remote player state')) { throw 'Host save/load failed' }
                $deathMarker = if ($role -eq 'host') { 'Triggered authoritative shared death' } else { 'Authoritative death applied on client' }
                $revivalMarker = if ($role -eq 'host') { 'Both players revived at host checkpoint' } else { 'Authoritative checkpoint revival applied on client' }
                $deathIndex=$log.IndexOf($deathMarker); $revivalIndex=$log.IndexOf($revivalMarker)
                if ($deathIndex -lt 0 -or $revivalIndex -le $deathIndex) { throw "$role failed ordered shared death/revival" }
            }
            if ($log.Contains('Rejected:') -or $log.Contains('readiness timed out')) { throw "$role replication failed; inspect log" }
            Write-Output "PASS: $role gameplay replication"
        }
    }
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    foreach ($p in $processes) { if (!$p.HasExited) { Stop-Process -Id $p.Id } }
}
