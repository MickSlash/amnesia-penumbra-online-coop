# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$Output = "$PSScriptRoot/../build/AmnesiaCoop.exe")
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force | Out-Null
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'CoopLauncher.cs')).Path
$target = [IO.Path]::GetFullPath($Output.Replace('/', '\'))
& $compiler /nologo /target:winexe /platform:x64 /optimize+ /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/out:$target" $source
if($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed' }
