# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two-player test on one PC: two game instances, two bridges over localhost.
#
#   A = your profile, host bridge   (OnlineCoop\<A>\)
#   B = test profile "CoopTestB", client bridge (OnlineCoop\CoopTestB\),
#       windowed 1280x720, starting from a copy of A's latest modded save.
#
# The avatar needs both players in the same map (never the menu), so press
# "Nuova partita" in both windows. Bridge logs (build/test/*.log) show, every
# 10 s, each game's map, feet position and its view of the other player's
# avatar. Close both games to end; the script then prints a summary.
param(
    [string]$GameDir = 'F:\SteamLibrary\steamapps\common\Amnesia The Bunker',
    [string]$ProfileB = 'CoopTestB',
    [int]$Minutes = 20
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$docs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games/Amnesia The Bunker'
if(-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Start Steam first.' }
if(Get-Process AmnesiaTheBunker -ErrorAction SilentlyContinue) { throw 'Close Amnesia: The Bunker first.' }

& (Join-Path $PSScriptRoot 'BuildBridge.ps1') | Out-Null
& (Join-Path $PSScriptRoot 'InstallMod.ps1') -GameDir $GameDir | Out-Null

# Profile A: the player's most recent non-test profile.
$settingsA = Get-ChildItem -LiteralPath (Join-Path $docs 'Main') -Filter '*_user_settings.cfg' |
    Where-Object { $_.Name -notlike 'CoopTest*' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if(-not $settingsA) { throw 'No game profile found: start the game normally once.' }
$profileA = $settingsA.Name -replace '_user_settings\.cfg$', ''

# Profile B: A's settings, windowed, so both windows fit on one screen.
$settings = Get-Content -Raw -LiteralPath $settingsA.FullName
$settings = [regex]::Replace($settings, '<Screen [^>]*/>', '<Screen FullScreen="false" Width="1280" Height="720" />')
Set-Content -Encoding utf8 -LiteralPath (Join-Path $docs "Main/$($ProfileB)_user_settings.cfg") -Value $settings

# B starts from a copy of A's newest modded save, so "Continua" puts both
# players in the same place.
$saveA = Get-ChildItem -LiteralPath (Join-Path $docs 'MainWithAddons') -Recurse -Filter '*.sav' -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Name -eq $profileA } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if($saveA) {
    $dirB = Join-Path $saveA.Directory.Parent.FullName $ProfileB
    New-Item -ItemType Directory -Force $dirB | Out-Null
    Copy-Item -LiteralPath $saveA.FullName -Destination (Join-Path $dirB $saveA.Name) -Force
    Write-Output "Save for B: $($saveA.Name)"
} else {
    Write-Output 'No modded save of A found: start a new game in both windows instead of Continua.'
}

# Bridges first, so each game finds its own folder when it starts.
$ipc = Join-Path $docs 'OnlineCoop'
$logs = Join-Path $root 'build/test'
New-Item -ItemType Directory -Force $logs | Out-Null
$bridge = Join-Path $root 'build/CoopBridge.exe'
$hostLog = Join-Path $logs 'bridge_A_host.log'; $clientLog = Join-Path $logs 'bridge_B_client.log'
$bridgeA = Start-Process -PassThru -WindowStyle Minimized -FilePath 'cmd.exe' -ArgumentList "/c `"`"$bridge`" --host --name A --ipc-dir `"$ipc\$profileA`" --minutes $Minutes > `"$hostLog`" 2>&1`""
$bridgeB = Start-Process -PassThru -WindowStyle Minimized -FilePath 'cmd.exe' -ArgumentList "/c `"`"$bridge`" --join 127.0.0.1 --name B --ipc-dir `"$ipc\$ProfileB`" --minutes $Minutes > `"$clientLog`" 2>&1`""
Start-Sleep -Seconds 2

# The game honours the batch only at its own location, the file its Mods
# menu writes before relaunching (a batch elsewhere is silently ignored).
$batch = Join-Path $docs 'Main/addons.batch'
"<AddOnBatch>`r`n    <AddOn Locator=`"local:OnlineCoop`" />`r`n</AddOnBatch>" | Set-Content -Encoding ascii -LiteralPath $batch
$env:SteamAppId = '1944430'; $env:SteamGameId = '1944430'
$exe = Join-Path $GameDir 'AmnesiaTheBunker.exe'
$gameA = Start-Process -PassThru -FilePath $exe -WorkingDirectory $GameDir -ArgumentList " -addonbatch `"$batch`" -user $profileA"
Start-Sleep -Seconds 10
$gameB = Start-Process -PassThru -FilePath $exe -WorkingDirectory $GameDir -ArgumentList " -addonbatch `"$batch`" -user $ProfileB"
Write-Output "Game A (profile $profileA, host) pid $($gameA.Id); game B (profile $ProfileB, client, windowed) pid $($gameB.Id)"
Write-Output 'Start "Nuova partita" in both windows (both begin at the start of the trenches; "Continua" may be unavailable for profile A). Close both games to finish.'

$gameA.WaitForExit(); $gameB.WaitForExit()
foreach($p in @($bridgeA, $bridgeB)) { if(-not $p.HasExited) { Get-CimInstance Win32_Process -Filter "ParentProcessId = $($p.Id)" | ForEach-Object { Stop-Process -Id $_.ProcessId -ErrorAction SilentlyContinue } } }
Write-Output '--- last state seen by the bridges ---'
foreach($log in @($hostLog, $clientLog)) {
    Write-Output (Split-Path $log -Leaf)
    Get-Content -LiteralPath $log | Select-String 'local game:|link=' | Select-Object -Last 2 | ForEach-Object { '  ' + $_.Line }
}
