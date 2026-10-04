param([int]$Port,[int]$Delay=35,[int]$DropEvery=20)
$ErrorActionPreference='Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'UdpRelay.cs')
[CoopUdpRelay]::Run($Port,$Delay,$DropEvery,55,1200)
