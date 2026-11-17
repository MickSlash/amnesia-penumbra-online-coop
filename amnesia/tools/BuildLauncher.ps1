# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$Output = "$PSScriptRoot/../build/AmnesiaCoop.exe")
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force | Out-Null
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'CoopLauncher.cs')).Path
$target = [IO.Path]::GetFullPath($Output.Replace('/', '\'))
# Launcher artwork (tools/assets). The executable only carries launcher_exe.ico
# (uncompressed bitmaps) as its Windows icon; the PNG-based icon and the splash
# are copied to assets/ beside it and loaded at run time. Embedding the
# compressed PNGs made the launcher look packed to antivirus heuristics.
$assets = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'assets')).Path
$exeIcon = Join-Path $assets 'launcher_exe.ico'
$artOut = Join-Path (Split-Path -Parent $target) 'assets'
New-Item -ItemType Directory -Path $artOut -Force | Out-Null
foreach($art in 'launcher.ico','launcher_splash.png') { Copy-Item -LiteralPath (Join-Path $assets $art) -Destination (Join-Path $artOut $art) -Force }
& $compiler /nologo /target:winexe /platform:x64 /optimize+ /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/win32icon:$exeIcon" "/out:$target" $source
if($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed' }
