# SPDX-License-Identifier: GPL-3.0-or-later
#
# Records how the game relaunches itself when a mod is started from its own
# Mods menu: the command line and the addons.batch it points to. That is the
# exact, official way to start the game with OnlineCoop enabled, which the
# test scripts then reuse without clicking through the menu.
#
#   ./CaptureLaunch.ps1     then, in the game: Mods > Online Co-op > Launch
param([int]$Minutes = 15)
$ErrorActionPreference = 'Stop'
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'build/launch-capture.txt'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$seen = @{}
$deadline = (Get-Date).AddMinutes($Minutes)
while((Get-Date) -lt $deadline) {
    foreach($p in Get-CimInstance Win32_Process -Filter "Name = 'AmnesiaTheBunker.exe'") {
        if($seen[$p.ProcessId]) { continue }
        $seen[$p.ProcessId] = $true
        $text = "pid $($p.ProcessId) at $(Get-Date -Format 'HH:mm:ss')`r`n$($p.CommandLine)"
        if($p.CommandLine -match '-addonbatch\s+"([^"]+)"' -and (Test-Path -LiteralPath $Matches[1])) {
            $text += "`r`n--- $($Matches[1])`r`n" + (Get-Content -Raw -LiteralPath $Matches[1])
        }
        Write-Output $text
        Add-Content -LiteralPath $out -Value ($text + "`r`n")
    }
    Start-Sleep -Milliseconds 300
}
