param([int]$Port,[int]$Delay=35,[int]$Jitter=0,[int]$DropEvery=20,[int]$Duration=55,[string]$StopFile="")
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'UdpRelay.cs')
[CoopUdpRelay]::Run($Port,$Delay,$Jitter,$DropEvery,$Duration,1200,$StopFile)
