param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [string]$Executable = "$PSScriptRoot/../../amnesia/src/game/x64/Release/Lux.exe",
    [int]$Port = 27032
)
$ErrorActionPreference = 'Stop'
$gamePath = (Resolve-Path -LiteralPath $GameDirectory).Path
$exePath = (Resolve-Path -LiteralPath $Executable).Path
$repoPath = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$runPath = Join-Path $repoPath 'build/game-smoke'
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
$environmentNames = @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_ADDRESS','AMNESIA_NET_SMOKE_SECONDS')
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    foreach ($role in @('host','client')) {
        $profile = "OnlineCoopSmoke_${runId}_$role"
        $profiles[$role] = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
        $init = $baseConfig -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
        $init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settingsPath`""
        $initPath = Join-Path $runPath "$role-init.cfg"
        $init | Set-Content -LiteralPath $initPath -Encoding utf8
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE', $role)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT', [string]$Port)
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS', '127.0.0.1')
        [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS', '15')
        $processes += Start-Process -FilePath $exePath -WorkingDirectory $gamePath -ArgumentList $initPath -WindowStyle Hidden -PassThru
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(50)
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
    }
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    foreach ($p in $processes) { if (!$p.HasExited) { Stop-Process -Id $p.Id } }
}
