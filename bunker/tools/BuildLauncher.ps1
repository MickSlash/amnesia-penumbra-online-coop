# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds bunker/build/BunkerCoop.exe: the launcher with the bridge core and
# the artwork shared with the Dark Descent launcher (amnesia/tools/assets).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$assets = (Resolve-Path (Join-Path $root '../amnesia/tools/assets')).Path
$icon = Join-Path $assets 'launcher.ico'
$splash = Join-Path $assets 'launcher_splash.png'
& $compiler /nologo /optimize+ /target:winexe /platform:x64 /r:System.Xml.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll `
    "/win32icon:$icon" "/resource:$icon,launcher.ico" "/resource:$splash,launcher_splash.png" `
    "/out:$(Join-Path $out 'BunkerCoop.exe')" (Join-Path $root 'launcher/BunkerLauncher.cs') (Join-Path $root 'bridge/BridgeCore.cs')
if($LASTEXITCODE -ne 0) { throw "csc failed with exit code $LASTEXITCODE" }
Write-Output "Built $(Join-Path $out 'BunkerCoop.exe')"
