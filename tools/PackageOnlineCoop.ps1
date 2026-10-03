# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$Executable = "$PSScriptRoot/../amnesia/src/game/x64/Release/Lux.exe")
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath "$PSScriptRoot/..").Path
$exe = (Resolve-Path -LiteralPath $Executable).Path
& "$PSScriptRoot/BuildLauncher.ps1"
if ($LASTEXITCODE -ne 0) { throw 'Could not build the graphical launcher.' }
$runtime = Join-Path (Split-Path -Parent $exe) 'OpenAL32.dll'
if (!(Test-Path -LiteralPath $runtime)) { throw 'Place the installed x64 OpenAL32.dll beside Lux.exe before packaging.' }
# Use a fresh directory; never overwrite an older package or include game data.
$destination = Join-Path $repo ('build/packages/AmnesiaOnlineCoop_' + [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss') + '_' + [Guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path "$destination/bin", "$destination/tools" -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination "$destination/bin/Lux.exe"
Copy-Item -LiteralPath "$repo/build/AmnesiaCoop.exe" -Destination "$destination/AmnesiaCoop.exe"
Copy-Item -LiteralPath $runtime -Destination "$destination/bin/OpenAL32.dll"
# The Creative router needs its software backend; OpenAL Soft needs no backend
# DLL. Prefer a sidecar, otherwise copy the installed x64 Windows backend.
$backend = Join-Path (Split-Path -Parent $exe) 'wrap_oal.dll'
if (!(Test-Path -LiteralPath $backend)) { $backend = Join-Path $env:WINDIR 'System32/wrap_oal.dll' }
if (Test-Path -LiteralPath $backend) {
    $backendBytes = [IO.File]::ReadAllBytes($backend)
    $backendOffset = if ($backendBytes.Length -gt 64) { [BitConverter]::ToInt32($backendBytes,60) } else { -1 }
    if ($backendOffset -ge 0 -and $backendOffset + 6 -le $backendBytes.Length -and [BitConverter]::ToUInt16($backendBytes,$backendOffset+4) -eq 0x8664) {
        Copy-Item -LiteralPath $backend -Destination "$destination/bin/wrap_oal.dll"
    }
}
Copy-Item -LiteralPath "$PSScriptRoot/StartOnlineCoop.ps1" -Destination "$destination/tools/StartOnlineCoop.ps1"
Copy-Item -LiteralPath "$repo/docs/online_installazione_it.md" -Destination "$destination/LEGGIMI.md"
Copy-Item -LiteralPath "$repo/LICENSE" -Destination "$destination/LICENSE"
$commit = (& git -c "safe.directory=$($repo.Replace('\','/'))" -C $repo rev-parse HEAD).Trim()
$manifest = [ordered]@{
    protocol = 6
    sourceCommit = $commit
    sourceRepository = 'https://github.com/TheBorn/Amnesia-The-Dark-Descent---Local-Co-op-Mod'
    builtExecutableSha256 = (Get-FileHash -LiteralPath "$destination/bin/Lux.exe" -Algorithm SHA256).Hash
    launcherSha256 = (Get-FileHash -LiteralPath "$destination/AmnesiaCoop.exe" -Algorithm SHA256).Hash
    steamIntegration = 'Optional App ID 57300 presence; original installed steam_api64.dll and active Steam license required; no Steam runtime redistributed.'
    openALSha256 = (Get-FileHash -LiteralPath "$destination/bin/OpenAL32.dll" -Algorithm SHA256).Hash
    openALBackendSha256 = $(if (Test-Path -LiteralPath "$destination/bin/wrap_oal.dll") { (Get-FileHash -LiteralPath "$destination/bin/wrap_oal.dll" -Algorithm SHA256).Hash } else { $null })
    scope = 'Experimental Windows x64; no game assets; see LEGGIMI.md for tested scope and installation.'
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath "$destination/manifest.json" -Encoding utf8
# GPL recipients get the exact tracked source including dependency notices.
& git -c "safe.directory=$($repo.Replace('\','/'))" -C $repo archive --format=zip "--output=$destination/source.zip" HEAD
if ($LASTEXITCODE -ne 0) { throw 'Could not archive the corresponding source.' }
Compress-Archive -LiteralPath $destination -DestinationPath "$destination.zip"
Write-Output "Package: $destination.zip"
Write-Output "Launcher: $destination/AmnesiaCoop.exe (double-click)"
