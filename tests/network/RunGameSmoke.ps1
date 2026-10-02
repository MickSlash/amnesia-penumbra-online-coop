param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [string]$Executable = "$PSScriptRoot/../../amnesia/src/game/x64/Release/Lux.exe",
    [int]$Port = 27032,
    [switch]$Replication,
    [switch]$Campaign
)
$ErrorActionPreference = 'Stop'
if ($Campaign) { $Replication = $true }
$gamePath = (Resolve-Path -LiteralPath $GameDirectory).Path
$exePath = (Resolve-Path -LiteralPath $Executable).Path
$repoPath = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$runPath = Join-Path $repoPath $(if ($Replication) { 'build/player-replication' } else { 'build/game-smoke' })
New-Item -ItemType Directory -Force -Path $runPath | Out-Null
$runId = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
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
$environmentNames = @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_ADDRESS','AMNESIA_NET_SMOKE_SECONDS','AMNESIA_NET_TEST_MOVEMENT','AMNESIA_NET_TEST_PICKUP','AMNESIA_NET_TEST_CAMPAIGN')
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    foreach ($role in @('host','client')) {
        $profile = "OnlineCoopSmoke_${runId}_$role"
        $profiles[$role] = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
        $init = $baseConfig -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settingsPath`""
        if ($Replication -and $role -eq 'host') {
            $hostSettings = Join-Path $runPath 'host-settings.cfg'
            ([IO.File]::ReadAllText($settingsPath) -replace 'ShowMenu="true"', 'ShowMenu="false"') | Set-Content -LiteralPath $hostSettings -Encoding utf8
            $init = $init.Replace($settingsPath, $hostSettings)
            $init = $init -replace 'File\s*=\s*"00_rainy_hall.map"', 'File = "02_entrance_hall.map"'
        }
        $initPath = Join-Path $runPath "$role-init.cfg"
        $init | Set-Content -LiteralPath $initPath -Encoding utf8
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE', $role)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT', [string]$Port)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS', '127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS', $(if ($Campaign) { '60' } elseif ($Replication) { '30' } else { '15' }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_MOVEMENT', $(if ($Replication -and $role -eq 'client') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_PICKUP', $(if ($Replication -and $role -eq 'host') { '1' } else { $null }))
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_CAMPAIGN', $(if ($Campaign -and $role -eq 'host') { '1' } else { $null }))
        $processes += Start-Process -FilePath $exePath -WorkingDirectory $gamePath -ArgumentList $initPath -WindowStyle Hidden -PassThru
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($(if ($Campaign) { 90 } else { 50 }))
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
        if (!$log.Contains('Protocol handshake accepted') -or !$log.Contains('Smoke test finished')) {
            throw "$role did not finish a handshake and clean smoke exit: $($profiles[$role])"
        }
        Write-Output "PASS: $role game handshake and clean exit ($($profiles[$role]))"
        if ($Replication) {
            $markers = if ($role -eq 'host') { @('Announced epoch=', 'Remote Player 2 moved under network input', 'Complete host entity baseline sent', 'Host validated remote pickup; exactly one tinderbox awarded') } else { @('Replica loaded epoch=', 'First authoritative player snapshot applied', 'Created host avatar in replica world', 'Complete host entity baseline applied', 'Client camera/body moved from authoritative snapshots', 'Host pickup destruction replicated to client') }
            foreach ($marker in $markers) { if (!$log.Contains($marker)) { throw "Missing $role replication proof: $marker" } }
            $inventoryMarker = if ($role -eq 'host') { 'Host validated remote inventory use; one potion consumed' } else { 'Shared host inventory applied' }
            if (!$log.Contains($inventoryMarker)) { throw "Missing $role inventory proof: $inventoryMarker" }
            if ($role -eq 'client' -and (!$log.Contains('Host scripted lighting applied') -or !$log.Contains('Host story message applied on client'))) { throw 'Client story/light presentation failed' }
            if ($Campaign) {
                if (!$log.Contains('epoch=3') -or !$log.Contains('01_old_archives.map')) { throw "$role failed campaign map/save synchronization" }
                if ($role -eq 'host' -and !$log.Contains('Host save/load retained remote player state')) { throw 'Host save/load failed' }
                $deathMarker = if ($role -eq 'host') { 'Both players revived at host checkpoint' } else { 'Authoritative checkpoint revival applied on client' }
                if (!$log.Contains($deathMarker)) { throw "$role failed shared death/revival" }
            }
            if ($log.Contains('Rejected:') -or $log.Contains('readiness timed out')) { throw "$role replication failed; inspect log" }
            Write-Output "PASS: $role gameplay replication"
        }
    }
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    foreach ($p in $processes) { if (!$p.HasExited) { Stop-Process -Id $p.Id } }
}
