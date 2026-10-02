param([Parameter(Mandatory=$true)][string]$GameDirectory, [int]$Port = 27033)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$game = (Resolve-Path -LiteralPath $GameDirectory).Path
$run = Join-Path $repo 'build/host-player-test'
New-Item -ItemType Directory -Force -Path $run | Out-Null
$profile = 'OnlineCoopPlayer_' + [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
$logPath = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Amnesia/$profile/hpl.log"
$settings = Join-Path $run 'settings.cfg'
@'
<Main ShowMenu="false" ShowPreMenu="false" SaveConfig="false" DefaultProfileName="net_test" UpdateLogActive="false" />
<Graphics TextureQuality="2" Shadows="0" SSAOActive="false" />
<Engine LimitFPS="true" />
<Screen Width="640" Height="480" FullScreen="false" Display="0" Vsync="false" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
'@ | Set-Content -LiteralPath $settings -Encoding utf8
$init = [IO.File]::ReadAllText((Join-Path $game 'config/main_init.cfg'))
$init = $init -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"$profile`""
$init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settings`""
$init = $init -replace 'File\s*=\s*"00_rainy_hall.map"', 'File = "02_entrance_hall.map"'
$initPath = Join-Path $run 'init.cfg'
$init | Set-Content -LiteralPath $initPath -Encoding utf8
$names = @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_SMOKE_SECONDS','AMNESIA_NET_TEST_DRIVE_FILE')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
$p = $null
function Read-LiveLog([string]$Path) {
    try {
        $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        $reader = [IO.StreamReader]::new($stream)
        try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
    } catch [IO.IOException] { return '' }
}
try {
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE','host')
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT',[string]$Port)
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_SMOKE_SECONDS','15')
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_TEST_DRIVE_FILE',$null)
    $p = Start-Process -FilePath "$repo/amnesia/src/game/x64/Release/Lux.exe" -WorkingDirectory $game -ArgumentList $initPath -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    $listening = $false
    while (!$p.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        if ((Test-Path -LiteralPath $logPath) -and (Read-LiveLog $logPath).Contains('Listening on')) { $listening = $true; break }
        Start-Sleep -Milliseconds 250
    }
    if (!$listening) { throw "Host never loaded the map/listened: $logPath" }
    & "$repo/build/network/NetworkTests.exe" move-client $Port
    if ($LASTEXITCODE -ne 0) { throw 'Remote input sender failed' }
    if (!$p.WaitForExit(20000)) { throw 'Game did not exit cleanly' }
    $p.Refresh()
    if ($p.ExitCode -ne 0) { throw "Game exit $($p.ExitCode)" }
    $log = [IO.File]::ReadAllText($logPath)
    [IO.File]::WriteAllText((Join-Path $run 'host.log'),$log)
    foreach ($marker in @('Created authoritative Player 2','Remote Player 2 moved under network input','Removed remote Player 2','Smoke test finished')) {
        if (!$log.Contains($marker)) { throw "Missing runtime proof: $marker ($logPath)" }
    }
    Write-Output "PASS: authoritative P2 spawn, network movement, disconnect cleanup and clean exit ($logPath)"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name]) }
    if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id }
}
