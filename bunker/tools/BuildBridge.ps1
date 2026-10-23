# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the OnlineCoop bridge with the C# compiler that ships with Windows
# (.NET Framework 4.x), so no SDK is needed. Output: bunker/build/CoopBridge.exe
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
& $compiler /nologo /optimize+ /target:exe "/out:$(Join-Path $out 'CoopBridge.exe')" /r:System.Xml.dll (Join-Path $root 'bridge/CoopBridge.cs') (Join-Path $root 'bridge/BridgeCore.cs')
if($LASTEXITCODE -ne 0) { throw "csc failed with exit code $LASTEXITCODE" }
Write-Output "Built $(Join-Path $out 'CoopBridge.exe')"
