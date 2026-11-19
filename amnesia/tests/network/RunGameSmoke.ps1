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
    [switch]$Effects,
    [switch]$Stairs,
    [switch]$Gate,
    [switch]$Levers,
    [switch]$Flashback,
    [switch]$LevelDoor,
    [switch]$Spawn,
    [switch]$Fairness,
    [switch]$InputGap,
    [switch]$Lean,
    [switch]$Door,
    [switch]$ItemPath,
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
if ($VisibilityProps -or $Effects -or $Stairs -or $Gate -or $Levers -or $Flashback -or $LevelDoor -or $Spawn -or $Fairness -or $InputGap -or $Lean -or $Door -or $ItemPath) { $MapOnly = $true }
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
$environmentNames = @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_ADDRESS','AMNESIA_NET_SMOKE_SECONDS','AMNESIA_NET_TEST_MOVEMENT','AMNESIA_NET_TEST_PICKUP','AMNESIA_NET_TEST_CAMPAIGN','AMNESIA_NET_TEST_ENEMY','AMNESIA_NET_TEST_MEDIA','AMNESIA_NET_TEST_DRIVE_FILE','AMNESIA_NET_TEST_RECOVERY','AMNESIA_NET_TEST_PREDICTION','AMNESIA_NET_TEST_SNAPSHOT_BLACKOUT','AMNESIA_NET_TEST_MENU','AMNESIA_NET_TEST_VISIBILITY_PROPS','AMNESIA_NET_TEST_EFFECTS','AMNESIA_NET_TEST_STAIRS','AMNESIA_NET_TEST_GATE','AMNESIA_NET_TEST_FLASHBACK','AMNESIA_NET_TEST_LEVEL_DOOR','AMNESIA_NET_TEST_SPAWN','AMNESIA_NET_TEST_FAIRNESS','AMNESIA_NET_TEST_INPUT_GAP','AMNESIA_NET_TEST_LEAN','AMNESIA_NET_TEST_DOOR','AMNESIA_NET_TEST_ITEM_PATH')
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
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_EFFECTS', $(if ($Effects) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_STAIRS', $(if ($Stairs) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_GATE', $(if ($Gate -and $role -eq 'host') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_LEVERS', $(if ($Levers) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_LEVERS_LOCAL', $(if ($Levers -and $env:AMNESIA_QA_LEVERS_LOCAL -eq '1' -and $role -eq 'host') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_FLASHBACK', $(if ($Flashback) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_LEVEL_DOOR', $(if ($LevelDoor -and $role -eq 'host') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_SPAWN', $(if ($Spawn -and $role -eq 'host') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_FAIRNESS', $(if ($Fairness) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_INPUT_GAP', $(if ($InputGap -and $role -eq 'client') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_LEAN', $(if ($Lean -and $role -eq 'client') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_DOOR', $(if ($Door) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_ITEM_PATH', $(if ($ItemPath) { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_DRIVE_FILE', $null)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT', [string]$(if ($role -eq 'client' -and $ClientPort -gt 0) { $ClientPort } else { $Port }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS', '127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS', $(if($Effects){'70'} elseif ($Campaign -or $MapOnly) { '60' } elseif ($Replication) { '30' } else { '15' }))
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
        if($Stairs) {
            $marker=if($role -eq 'client'){'PASS client stair ascent and descent'}else{'PASS host authoritative stair ascent and descent'}
            if(!$log.Contains($marker) -or $log.Contains('FAILED stairs')) {throw "Missing $role stairs regression proof"}
            if($role -eq 'host' -and !$log.Contains('PASS missing footstep data safely initialized')) {throw 'Missing sound failure safety proof'}
            Write-Output "PASS: $role native staircase ascent, descent and clean exit"
        }
        if($Lean -and $role -eq 'client') {
            if(!$log.Contains('PASS client lean reached the camera')) {throw 'Client lean did not reach the camera'}
            if($log.Contains('Client predicted item pickup hidden=')) {
                if(!$log.Contains('Client predicted item pickup hidden=1') -or !$log.Contains('PASS client predicted pickup shown again')) {throw 'Client predicted pickup did not hide and come back'}
                Write-Output 'PASS: client item pickup shown at once and restored when the host kept the item'
            }
            Write-Output 'PASS: client lean (E) applied by the host and shown on the client camera'
        }
        if($ItemPath) {
            # A save-inflated item image must load with one folder prefix and still reach the client.
            $marker = if($role -eq 'host') { 'Host restored item from an inflated save path; image=graphics/item/lantern.tga' } else { 'PASS client received item restored from an inflated save path; image=graphics/item/lantern.tga' }
            if(!$log.Contains($marker) -or $log.Contains('Cannot encode inventory')) {throw "Missing $role item path proof: $marker"}
            if($role -eq 'client' -and !$log.Contains('PASS client received the original bucket_of_tar.ent item; image=graphics/item/bucket_of_tar.tga')) {throw 'Client did not receive the original bucket_of_tar item'}
            Write-Output "PASS: $role item from an inflated save path shared without stalling the client"
        }
        if($Door -and $role -eq 'client') {
            # The client must show the door where the host's physics left it.
            $hostLog = [IO.File]::ReadAllText((Join-Path $runPath 'host.log'))
            $h = [regex]::Match($hostLog, 'Door pose role=host yaw=(-?[\d.]+) closed=(\d) opened_deg=([\d.]+)')
            $c = [regex]::Match($log, 'Door pose role=client yaw=(-?[\d.]+) closed=(\d)')
            if(!$h.Success -or !$c.Success) {throw 'Missing door pose from host or client'}
            if([double]$h.Groups[3].Value -lt 15) {throw "Host door did not open (opened_deg=$($h.Groups[3].Value))"}
            $delta = [math]::Abs([double]$h.Groups[1].Value - [double]$c.Groups[1].Value) % 360
            $delta = [math]::Min($delta, 360 - $delta)
            Write-Output "INFO: door host yaw=$($h.Groups[1].Value) closed=$($h.Groups[2].Value); client yaw=$($c.Groups[1].Value) closed=$($c.Groups[2].Value)"
            if($delta -gt 3 -or $h.Groups[2].Value -ne $c.Groups[2].Value) {throw "Client door differs from host by $delta deg"}
            Write-Output "PASS: client door rests where the host's physics left it (opened $($h.Groups[3].Value) deg)"
            # Then the host breaks it: the client must hide the leaf and show the debris.
            if(!$hostLog.Contains('Door broken role=host broken=1 leaf_active=0 debris=1')) {throw 'Host door fixture did not break'}
            if(!$log.Contains('Door broken role=client broken=1 leaf_active=0 debris=1')) {throw 'Client still shows the door the host broke'}
            Write-Output 'PASS: door broken on the host is broken on the client (leaf hidden, debris replicated)'
        }
        if($InputGap -and $role -eq 'host') {
            $gaps = [regex]::Matches($log, 'Client input resumed after a (\d+) ms gap') | ForEach-Object { [int]$_.Groups[1].Value }
            $pauses = ([regex]::Matches($log, 'No client input for 3 s; campaign paused')).Count
            if(@($gaps | Where-Object { $_ -ge 1500 -and $_ -lt 3000 }).Count -lt 1) {throw 'Short input gap was not logged'}
            if($pauses -ne 1 -or !$log.Contains('Client input returned; campaign resumed')) {throw "Expected exactly one pause (long gap only), got $pauses"}
            Write-Output 'PASS: host kept running through a 2 s input gap and paused only for the 4 s one'
        }
        if($Fairness) {
            $marker = if($role -eq 'host') { 'Host moved the late target once' } else { 'PASS late entity update reached client despite 80 moving props' }
            if(!$log.Contains($marker)) {throw "Missing $role fairness proof: $marker"}
            Write-Output "PASS: $role late entity update not starved by 80 moving props"
        }
        if($Spawn) {
            if($role -eq 'host' -and (!$log.Contains('Remote spawn fallback: sharing the host position crouched') -or !$log.Contains('Host crouched in a closed crawl space'))) {throw 'Host did not use the crouched spawn fallback'}
            if($log.Contains('No safe remote spawn') -or ($role -eq 'client' -and $log.Contains('Connection lost; campaign paused for recovery'))) {throw "$role session dropped while respawning Player 2"}
            Write-Output "PASS: $role Player 2 respawned crouched beside a boxed-in host"
        }
        if($LevelDoor) {
            $marker = if($role -eq 'host') { 'Announced epoch=2 file=maps/main/09_back_hall.map' } else { 'Replica loaded epoch=2 file=maps/main/09_back_hall.map' }
            if(!$log.Contains($marker) -or $log.Contains('Cannot announce map')) {throw "Missing $role level door proof: $marker"}
            if($role -eq 'client') {
                foreach($notice in @('Client showed waiting for host notice','Client showed host loading notice')) { if(!$log.Contains($notice)) {throw "Missing client notice: $notice"} }
                if($log.Contains('Connection lost; campaign paused for recovery')) {throw 'Client dropped the session during the host stall or load'}
            }
            Write-Output "PASS: $role level door with authoring path reached 09_back_hall"
        }
        if($Flashback) {
            $markers = if($role -eq 'client') { @('PASS flashback sepia applied on client') } else { @('Host flashback started','PASS flashback slows the remote player too','PASS removing a held item returns its holder to normal','PASS destroyed GUI element drawn for 30 frames without crashing') }
            foreach($marker in $markers) { if(!$log.Contains($marker)) {throw "Missing $role flashback/GUI proof: $marker"} }
            if($role -eq 'host' -and (!($log -match 'skipped \d+ draw of destroyed') -or !($log -match 'skipped \d+ render of destroyed'))) {throw 'Destroyed GUI elements were not skipped at draw and render'}
            if($log.Contains('FAILED held item')) {throw 'Held item regression failed'}
            Write-Output "PASS: $role flashback effects and GUI lifetime regressions"
        }
        if($Levers -and $role -eq 'client') {
            foreach($marker in @('PASS client sees the lever move','PASS client sees the wheel move')) { if(!$log.Contains($marker)) {throw "Missing client lever/wheel view proof: $marker"} }
            Write-Output 'PASS: client sees the lever and the wheel move'
        }
        if($Levers -and $role -eq 'host') {
            foreach($marker in @('PASS client pulled the lever','PASS client turned the wheel')) { if(!$log.Contains($marker)) {throw "Missing lever/wheel proof: $marker"} }
            if($log.Contains('FAILED lever') -or $log.Contains('FAILED client')) {throw 'Client lever/wheel interaction failed'}
            Write-Output 'PASS: client pulls a lever and turns a wheel through the network'
        }
        if($Gate -and $role -eq 'host') {
            foreach($marker in @('PASS gate stays open while one player is past it','PASS gate closes once both players passed')) { if(!$log.Contains($marker)) {throw "Missing lever gate proof: $marker"} }
            if($log.Contains('FAILED gate')) {throw 'Lever gate regression failed'}
            Write-Output 'PASS: host lever gate waits for both players and then closes'
        }
        if($Effects) {
            if(!$log.Contains('[NET/TEST] PASS objective flash and emotion page recovery') -or $log.Contains('[NET/TEST] FAILED effects')) {throw "Missing $role effects regression proof"}
            if($role -eq 'client' -and !$log.Contains('Blue objective flash visible on local client viewport')) {throw 'Client did not render objective flash'}
            $jumpMarker=if($role -eq 'client'){'PASS client jump prediction and landing'}else{'PASS authoritative jump and players pass through'}
            if(!$log.Contains($jumpMarker)){throw "Missing $role jump/collision regression proof"}
            if(!$log.Contains("PASS $role respects scripted jump disable")){throw "Missing $role scripted jump permission proof"}
            Write-Output "PASS: $role objective flash, emotion page advance, natural expiry and restored controls"
        }
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
                $enemyMarkers = if($role -eq 'host') { @('Spawned authoritative enemy fixture','Enemy occlusion and switching between both players verified','Host enemy moved under authoritative AI','Host enemy caused player damage') } else { @('Authoritative enemy created on client','Authoritative enemy movement applied on client','Enemy rendered between authoritative poses','Host enemy damage applied on client') }
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
