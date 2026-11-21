# SPDX-License-Identifier: GPL-3.0-or-later
# Prova nativa su due PC: host su questo PC, client su un secondo PC raggiunto
# via SSH (solo per il controllo) e collegato alla partita tramite l'indirizzo
# indicato (per esempio quello Radmin VPN di questo PC).
#
# Sul secondo PC servono: OpenSSH Server con accesso a chiave, l'utente
# collegato (console o Desktop Remoto, per avere la scheda video) e il gioco
# installato. L'exe e il runtime Visual C++ x86 vengono copiati in
# <RemoteDir>\bin; i dati del gioco non vengono toccati.
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,      # redist locale
    [Parameter(Mandatory=$true)][string]$Remote,             # utente@indirizzo LAN
    [Parameter(Mandatory=$true)][string]$RemoteGameDirectory,# redist sul secondo PC
    [Parameter(Mandatory=$true)][string]$HostAddress,        # indirizzo di questo PC per la partita
    [string]$RemoteDir = '',                                 # predefinita: Desktop\PenumbraCoopTest
    [string]$Key = "$env:USERPROFILE\.ssh\id_ed25519_penumbra_test",
    [ValidateSet('baseline','full','hammer','campaign','resume')][string]$Mode = 'full',
    [int]$Seconds = 0,
    [string]$PreviousRun = '',
    [string]$ClientSettings = '',                            # settings.cfg da usare sul client
    [string]$ClientSize = '640x480',
    [switch]$ClientFullscreen,
    [switch]$ManualClient,                                   # il client lo avvia una persona
    [switch]$SkipDeploy,                                     # exe già copiato (rete lenta o instabile)
    [int]$Port = 27120
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$ssh = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15')
$user = $Remote.Split('@')[0]
if (!$RemoteDir) { $RemoteDir = "C:\Users\$user\Desktop\PenumbraCoopTest" }
$remoteSlash = $RemoteDir.Replace('\', '/')
if (!$Seconds) { $Seconds = @{ baseline = 50; full = 90; hammer = 110; campaign = 180; resume = 70 }[$Mode] }
$id = 'twopc-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$run = Join-Path $root "build\qa\$id"
New-Item -ItemType Directory -Force $run | Out-Null
$rdir = "$RemoteDir\runs\$id"
$resume = if ($Mode -eq 'resume') { '1' } else { '0' }
if ($Mode -eq 'resume') {
    if (!$PreviousRun) { throw 'La prova resume richiede PreviousRun' }
    Copy-Item -LiteralPath (Join-Path $PreviousRun 'host') -Destination (Join-Path $run 'host') -Recurse
}

# Exe e runtime Visual C++ x86 accanto all'exe (distribuzione locale ammessa da Microsoft).
$exe = Join-Path $root 'build\bin\Release\PenumbraCoop.exe'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Recurse -Filter vcruntime140.dll |
    Where-Object { $_.FullName -match '\\x86\\Microsoft\.VC\d+\.CRT\\' -and $_.FullName -notmatch 'onecore' } | Select-Object -First 1
if (!$SkipDeploy) {
    & ssh @ssh $Remote "if not exist `"$RemoteDir\bin`" mkdir `"$RemoteDir\bin`"" | Out-Null
    & scp -q -i $Key $exe (Join-Path $crt.DirectoryName 'vcruntime140.dll') (Join-Path $crt.DirectoryName 'msvcp140.dll') "${Remote}:$remoteSlash/bin/"
    if ($LASTEXITCODE -ne 0) { throw 'Copia dell''exe sul secondo PC non riuscita' }
}

$settings = ''
if ($ClientSettings) {
    & scp -q -i $Key $ClientSettings "${Remote}:$remoteSlash/client_settings.cfg"
    $settings = "mkdir `"$rdir\client\PenumbraOnlineCoop\Overture`" 2>nul`r`ncopy /Y `"$RemoteDir\client_settings.cfg`" `"$rdir\client\PenumbraOnlineCoop\Overture\settings.cfg`" >nul"
}
$size = $ClientSize.Split('x')
$cmd = @"
@echo off
set PENUMBRA_COOP_QA=$Mode
set PENUMBRA_COOP_ROLE=client
set PENUMBRA_COOP_ADDRESS=$HostAddress
set PENUMBRA_COOP_PORT=$Port
set PENUMBRA_COOP_QA_SIMPLE=0
set PENUMBRA_COOP_RESUME=$resume
set PENUMBRA_COOP_QA_WIDTH=$($size[0])
set PENUMBRA_COOP_QA_HEIGHT=$($size[1])
set PENUMBRA_COOP_QA_FULLSCREEN=$(if ($ClientFullscreen) { '1' } else { '0' })
set PENUMBRA_COOP_FINAL_SCREENSHOT=$rdir\client_final.bmp
set PENUMBRA_COOP_PROFILE_ROOT=$rdir\client
set PENUMBRA_COOP_SCREENSHOT=$rdir\client.bmp
mkdir "$rdir" 2>nul
$settings
cd /d "$RemoteGameDirectory"
"$RemoteDir\bin\PenumbraCoop.exe"
echo exit=%ERRORLEVEL% > "$rdir\exit.txt"
"@
$cmdFile = Join-Path $run 'run_client.cmd'
[IO.File]::WriteAllText($cmdFile, $cmd.Replace("`r`n", "`n").Replace("`n", "`r`n"), [Text.Encoding]::ASCII)
& scp -q -i $Key $cmdFile "${Remote}:$remoteSlash/run_client.cmd"
# Attività interattiva: il client gira nella sessione dell'utente collegato.
& ssh @ssh $Remote "schtasks /Create /TN PenumbraCoopClient /TR `"$RemoteDir\run_client.cmd`" /SC ONCE /ST 23:59 /IT /F" | Out-Null

$env:PENUMBRA_COOP_QA = $Mode
$env:PENUMBRA_COOP_PORT = "$Port"
$env:PENUMBRA_COOP_RESUME = $resume
$env:PENUMBRA_COOP_ROLE = 'host'
$env:PENUMBRA_COOP_PLAYERS = '2'
$env:PENUMBRA_COOP_PROFILE_ROOT = Join-Path $run 'host'
$env:PENUMBRA_COOP_SCREENSHOT = Join-Path $run 'host.bmp'
$env:PENUMBRA_COOP_FINAL_SCREENSHOT = Join-Path $run 'host_final.bmp'
$hostProcess = Start-Process -FilePath $exe -WorkingDirectory $GameDirectory -WindowStyle Hidden -PassThru
$null = $hostProcess.Handle
Start-Sleep -Seconds 3
if ($ManualClient) { Write-Output "Avviare a mano $RemoteDir\run_client.cmd sul secondo PC" }
else { & ssh @ssh $Remote 'schtasks /Run /TN PenumbraCoopClient' | Out-Null }

$deadline = (Get-Date).AddSeconds($Seconds + $(if ($ManualClient) { 240 } else { 0 }))
while ((Get-Date) -lt $deadline -and !$hostProcess.HasExited) { Start-Sleep -Milliseconds 500 }
$passed = $true
if (!$hostProcess.HasExited) { Stop-Process -Id $hostProcess.Id -Force; $passed = $false; Write-Output 'Host fermato per tempo scaduto' }
$hostProcess.Refresh(); if ($hostProcess.ExitCode -ne 0) { $passed = $false }
for ($i = 0; $i -lt 20; ++$i) {
    $alive = & ssh @ssh $Remote 'tasklist /NH | findstr /I PenumbraCoop'
    if (!($alive -match 'PenumbraCoop')) { break }
    Start-Sleep -Seconds 1
}
if ($alive -match 'PenumbraCoop') { & ssh @ssh $Remote 'taskkill /F /IM PenumbraCoop.exe' | Out-Null; $passed = $false; Write-Output 'Client fermato per tempo scaduto' }
New-Item -ItemType Directory -Force (Join-Path $run 'client') | Out-Null
& scp -q -r -i $Key "${Remote}:$remoteSlash/runs/$id/*" (Join-Path $run 'client') 2>$null
foreach ($role in 'host', 'client') {
    Write-Output "== $role"
    $log = Get-ChildItem -Recurse (Join-Path $run $role) -Filter hpl.log -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$log) { $passed = $false; Write-Output 'log mancante'; continue }
    Select-String -Path $log.FullName -Pattern '\[COOP/QA\]|FATAL|NET/CONNECTION' | ForEach-Object Line
    if (!(Select-String -Path $log.FullName -Pattern '\[COOP/QA\] PASS' -Quiet)) { $passed = $false }
}
Write-Output $run
if (!$passed) { throw 'Prova su due PC fallita; log conservati' }
