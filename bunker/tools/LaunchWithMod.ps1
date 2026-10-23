# SPDX-License-Identifier: GPL-3.0-or-later
#
# Starts the game with the OnlineCoop add-on enabled, exactly as the game's
# own Mods menu relaunches itself (recorded with CaptureLaunch.ps1):
#
#   AmnesiaTheBunker.exe -addonbatch "<file>" -user <profile>
#   <AddOnBatch><AddOn Locator="local:<mod folder name>" /></AddOnBatch>
#
# Uses the Steam executable; Steam must be running.
param(
    [string]$GameDir = 'F:\SteamLibrary\steamapps\common\Amnesia The Bunker',
    # Profile folder name; defaults to the only/most recent profile found.
    [string]$User = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$profiles = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games/Amnesia The Bunker/Main'
if(-not $User) {
    # CoopTest* profiles belong to the two-instance tests, never to the player.
    $settings = Get-ChildItem -LiteralPath $profiles -Filter '*_user_settings.cfg' | Where-Object { $_.Name -notlike 'CoopTest*' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if(-not $settings) { throw "No game profile found in $profiles; start the game once normally." }
    $User = $settings.Name -replace '_user_settings\.cfg$', ''
}
# The game honours the batch only at its own location, the file its Mods
# menu writes before relaunching (a batch elsewhere is silently ignored).
$batch = Join-Path $profiles 'addons.batch'
"<AddOnBatch>`r`n    <AddOn Locator=`"local:OnlineCoop`" />`r`n</AddOnBatch>" | Set-Content -Encoding ascii -LiteralPath $batch
# Same launch as the game's own relaunch: the exe with SteamAppId in the
# environment (Steam sets it for the games it starts, and the relaunched
# child inherits it). Without it Steam restarts the game and drops the
# arguments. Steam must be running: the game still
# initialises the Steam API and checks the licence as usual.
if(-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Start Steam first.' }
$env:SteamAppId = '1944430'
$env:SteamGameId = '1944430'
$proc = Start-Process -PassThru -FilePath (Join-Path $GameDir 'AmnesiaTheBunker.exe') -WorkingDirectory $GameDir `
    -ArgumentList " -addonbatch `"$batch`" -user $User"
Write-Output "Started pid $($proc.Id) with OnlineCoop (profile $User)"
