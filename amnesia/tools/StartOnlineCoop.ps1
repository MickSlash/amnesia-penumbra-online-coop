# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][ValidateSet('Host','Client')][string]$Role,
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [string]$Address = '127.0.0.1',
    [ValidateRange(1,65535)][int]$Port = 27020,
    [string]$Executable = "$PSScriptRoot/../amnesia/src/game/x64/Release/Lux.exe",
    [ValidateRange(640,7680)][int]$Width = 1280,
    [ValidateRange(480,4320)][int]$Height = 720,
    [switch]$FullScreen
)
$ErrorActionPreference = 'Stop'
if (!$PSBoundParameters.ContainsKey('Executable') -and !(Test-Path -LiteralPath $Executable) -and (Test-Path -LiteralPath "$PSScriptRoot/../bin/Lux.exe")) {
    $Executable = "$PSScriptRoot/../bin/Lux.exe"
}
$gamePath = (Resolve-Path -LiteralPath $GameDirectory).Path
$exePath = (Resolve-Path -LiteralPath $Executable).Path
foreach ($required in @('config/main_init.cfg','maps','entities','resources.cfg')) {
    if (!(Test-Path -LiteralPath (Join-Path $gamePath $required))) { throw "Missing legitimate game data: $required" }
}
$runPath = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/..").Path "build/online-launch/$Role"
New-Item -ItemType Directory -Path $runPath -Force | Out-Null
$settingsPath = Join-Path $runPath 'settings.cfg'
$show = 'true' # Connect both peers in the menu before the host starts/continues the campaign.
$full = $FullScreen.IsPresent.ToString().ToLowerInvariant()
@"
<Main ShowMenu="$show" ShowPreMenu="false" SaveConfig="false" DefaultProfileName="online" UpdateLogActive="false" />
<Graphics TextureQuality="0" Shadows="1" SSAOActive="true" />
<Engine LimitFPS="true" />
<Screen Width="$Width" Height="$Height" Display="0" FullScreen="$full" Vsync="true" />
<Physics PhysicsAccuracy="2" UpdatesPerSec="60" />
"@ | Set-Content -LiteralPath $settingsPath -Encoding utf8
$init = [IO.File]::ReadAllText((Join-Path $gamePath 'config/main_init.cfg'))
$init = $init -replace 'MainSaveFolder\s*=\s*"[^"]*"', "MainSaveFolder = `"AmnesiaOnlineCoop_$Role`""
$settingsEscaped = [Security.SecurityElement]::Escape($settingsPath)
$init = $init -replace 'DefaultMainSettings(SDL2)?\s*=\s*"[^"]*"', "DefaultMainSettings`$1 = `"$settingsEscaped`""
$initPath = Join-Path $runPath 'init.cfg'
$init | Set-Content -LiteralPath $initPath -Encoding utf8
# The original game may contain an x86 DLL. Use the installed x64 OpenAL
# runtime beside this executable when it is available; never copy game assets.
$openAL = Join-Path (Split-Path -Parent $exePath) 'OpenAL32.dll'
if (!(Test-Path -LiteralPath $openAL)) {
    $systemOpenAL = Join-Path $env:WINDIR 'System32/OpenAL32.dll'
    if (Test-Path -LiteralPath $systemOpenAL) {
        $bytes = [IO.File]::ReadAllBytes($systemOpenAL)
        if ($bytes.Length -gt 64) {
            $peOffset = [BitConverter]::ToInt32($bytes,60)
            if ($peOffset -ge 0 -and $peOffset + 6 -le $bytes.Length -and [BitConverter]::ToUInt16($bytes,$peOffset+4) -eq 0x8664) {
                Copy-Item -LiteralPath $systemOpenAL -Destination $openAL
            }
        }
    }
}
$names = @('AMNESIA_NET_ROLE','AMNESIA_NET_PORT','AMNESIA_NET_ADDRESS','AMNESIA_NET_SMOKE_SECONDS','AMNESIA_NET_TEST_MOVEMENT','AMNESIA_NET_TEST_PICKUP','AMNESIA_NET_TEST_CAMPAIGN','AMNESIA_NET_TEST_ENEMY','AMNESIA_NET_TEST_MEDIA','AMNESIA_NET_TEST_DRIVE_FILE','AMNESIA_NET_TEST_RECOVERY','AMNESIA_NET_TEST_PREDICTION')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
$previousDirectory = (Get-Location).Path
try {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$null) }
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_ROLE',$Role.ToLowerInvariant())
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_PORT',[string]$Port)
    [Environment]::SetEnvironmentVariable('AMNESIA_NET_ADDRESS',$Address)
    Set-Location -LiteralPath $gamePath
    Write-Output "$Role on UDP $Port; saves use AmnesiaOnlineCoop_$Role. Client waits for the host's level."
    & $exePath $initPath
    if ($LASTEXITCODE -ne 0) { throw "Game exited with $LASTEXITCODE; inspect the role's hpl.log in Documents/Amnesia." }
} finally {
    Set-Location -LiteralPath $previousDirectory
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name]) }
}
