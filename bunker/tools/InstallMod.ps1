# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copies the OnlineCoop add-on into the game's mods folder. Game files are
# never touched: the add-on lives in its own folder and overrides scripts by
# name only while it is enabled in the game's Mods menu. A previous copy of
# the add-on is moved to bunker/backups first.
#
#   ./InstallMod.ps1                       install / update
#   ./InstallMod.ps1 -Uninstall            remove (after a backup)
param(
    [string]$GameDir = 'F:\SteamLibrary\steamapps\common\Amnesia The Bunker',
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'mod/OnlineCoop'
$mods = Join-Path $GameDir 'mods'
$target = Join-Path $mods 'OnlineCoop'
if(-not (Test-Path (Join-Path $GameDir 'AmnesiaTheBunker.exe'))) { throw "Amnesia: The Bunker not found in '$GameDir'" }

if(Test-Path $target) {
    $backup = Join-Path $root ('backups/OnlineCoop_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
    New-Item -ItemType Directory -Force (Split-Path $backup) | Out-Null
    Move-Item -LiteralPath $target -Destination $backup
    Write-Output "Previous add-on moved to $backup"
}
if($Uninstall) { Write-Output 'OnlineCoop add-on removed.'; return }

Copy-Item -LiteralPath $source -Destination $target -Recurse
# The avatar's animations are private copies of the player's own, taken
# from this installation (see tools/MakePartnerEntity.py): shared files
# would break the real player's animations.
foreach($line in Get-Content -LiteralPath (Join-Path $target 'entities/coop/anims.txt')) {
    if($line -match '^\s*(#|$)') { continue }
    $from, $to = $line -split '\|'
    $dest = Join-Path $target $to
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Copy-Item -LiteralPath (Join-Path $GameDir $from) -Destination $dest
}
Write-Output "OnlineCoop add-on installed in $target"
Write-Output 'Enable it from the game: main menu > Mods > select "Online Co-op (experimental)" > Launch.'
