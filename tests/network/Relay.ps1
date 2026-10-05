param([int]$Port,[int]$Delay=35,[int]$DropEvery=20,[int]$Duration=55)
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'UdpRelay.cs')
[CoopUdpRelay]::Run($Port,$Delay,$DropEvery,$Duration,1200)
