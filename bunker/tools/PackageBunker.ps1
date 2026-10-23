# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds launcher + bridge and zips what a player needs:
#   BunkerCoop.exe      launcher (bridge included)
#   CoopBridge.exe      command-line bridge, for diagnostics
#   mod/OnlineCoop/     the add-on (the launcher installs it in the game)
#   LEGGIMI.txt
# Output: bunker/build/packages/BunkerOnlineCoop_<timestamp>.zip
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'BuildLauncher.ps1') | Out-Null
& (Join-Path $PSScriptRoot 'BuildBridge.ps1') | Out-Null
$stage = Join-Path $root ('build/stage_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force (Join-Path $stage 'mod') | Out-Null
Copy-Item (Join-Path $root 'build/BunkerCoop.exe'), (Join-Path $root 'build/CoopBridge.exe') $stage
Copy-Item (Join-Path $root 'mod/OnlineCoop') (Join-Path $stage 'mod/OnlineCoop') -Recurse
@'
AMNESIA: THE BUNKER - CO-OP ONLINE (sperimentale)

1. Estrai tutta la cartella dello ZIP (non avviare dall'interno dello ZIP).
2. Apri Steam con l'account che possiede Amnesia: The Bunker.
3. Avvia BunkerCoop.exe.
   - Chi ospita sceglie "Ospita la partita" e comunica il proprio indirizzo
     (per esempio quello di Radmin VPN, 26.x.x.x) all'altro giocatore.
   - Chi entra sceglie "Entra nella partita di:" e scrive quell'indirizzo.
   Se Windows chiede il permesso del firewall, consenti l'accesso.
4. Il gioco si apre con la mod gia' attiva: carica o inizia una partita.
   La finestra del launcher mostra lo stato del collegamento e il ping.

La mod non modifica i file del gioco: aggiunge la cartella mods\OnlineCoop.
Senza il launcher aperto il gioco funziona normalmente.
'@ | Set-Content -Encoding utf8 (Join-Path $stage 'LEGGIMI.txt')
$packages = Join-Path $root 'build/packages'
New-Item -ItemType Directory -Force $packages | Out-Null
$zip = Join-Path $packages ('BunkerOnlineCoop_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.zip')
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force -LiteralPath $stage
Write-Output "Package: $zip"
