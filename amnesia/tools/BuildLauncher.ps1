# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$Output = "$PSScriptRoot/../build/AmnesiaCoop.exe")
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force | Out-Null
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'CoopLauncher.cs')).Path
$target = [IO.Path]::GetFullPath($Output.Replace('/', '\'))
# Launcher artwork (tools/assets): the .ico becomes the executable's icon and,
# with the splash PNG, an embedded resource read by LauncherArt at run time.
$assets = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'assets')).Path
$icon = Join-Path $assets 'launcher.ico'
$splash = Join-Path $assets 'launcher_splash.png'
& $compiler /nologo /target:winexe /platform:x64 /optimize+ /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/win32icon:$icon" "/resource:$icon,launcher.ico" "/resource:$splash,launcher_splash.png" "/out:$target" $source
if($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed' }
