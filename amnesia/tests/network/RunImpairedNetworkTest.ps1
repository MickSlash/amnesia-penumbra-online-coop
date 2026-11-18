# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [ValidateRange(1,65534)][int]$Port = 27036,
    [ValidateRange(0,500)][int]$OneWayDelayMs = 50,
    [ValidateRange(2,1000)][int]$DropEvery = 20,
    # Ritardo casuale aggiuntivo per pacchetto (0..JitterMs): i pacchetti
    # possono arrivare raggruppati o fuori ordine, come su una rete reale.
    [ValidateRange(0,200)][int]$JitterMs = 0,
    [switch]$Media,
    [switch]$Prediction,
    [switch]$SnapshotBlackout,
    [switch]$OnlineMenu,
    [switch]$VisibilityProps,
    [switch]$Effects,
    [switch]$Stairs,
    [switch]$Gate,
    [switch]$Levers,
    [switch]$Flashback,
    [switch]$LevelDoor,
    [switch]$Spawn,
    [switch]$Fairness,
    [switch]$InputGap,
    [ValidatePattern('^[a-zA-Z0-9_-]+\.map$')][string]$StartMap = '02_entrance_hall.map',
    [switch]$MapOnly,
    [ValidateRange(576,65507)][int]$MaxDatagramBytes = 65507,
    [string]$Executable = "$PSScriptRoot/../../amnesia/src/game/x64/Release/Lux.exe"
)
$ErrorActionPreference = 'Stop'
if($OnlineMenu -or $Effects -or $Stairs -or $Gate -or $Levers -or $Flashback -or $LevelDoor -or $Spawn -or $Fairness -or $InputGap) { $MapOnly = $true }
$run = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path 'build/impaired-network'
$gameLogs = Join-Path "$PSScriptRoot/../../build" $(if($OnlineMenu) { 'online-menu' } elseif($MapOnly -or $VisibilityProps) { "visual-qa/$($StartMap.Replace('.map',''))" } else { 'player-replication' })
New-Item -ItemType Directory -Force -Path $run | Out-Null
$relaySource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;
public static class CoopUdpRelay {
    class Pending { public byte[] Data; public EndPoint Target; public Socket Socket; public long Due; }
    // Firma senza jitter, usata da RunCampaignStartTest e RunLevelNetworkProbe.
    public static string Run(int port, int delay, int dropEvery, int seconds, int ceiling) { return Run(port, delay, 0, dropEvery, seconds, ceiling); }
    public static string Run(int port, int delay, int jitter, int dropEvery, int seconds, int ceiling) {
        using(var front = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
        using(var back = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp)) {
            front.Bind(new IPEndPoint(IPAddress.Loopback, port));
            back.Bind(new IPEndPoint(IPAddress.Loopback, 0));
            EndPoint server = new IPEndPoint(IPAddress.Loopback, port+1), client = null;
            var queue = new List<Pending>(); var clock = Stopwatch.StartNew(); var random = new Random(1234);
            var buffer = new byte[65536]; long received = 0, dropped = 0, forwarded = 0, bytes = 0;
            long oversized = 0; int largest = 0, largestForwarded = 0;
            while(clock.ElapsedMilliseconds < seconds*1000L) {
                for(int side=0; side<2; ++side) {
                    var socket = side==0 ? front : back;
                    for(int count=0; count<64; ++count) {
                        EndPoint source = new IPEndPoint(IPAddress.Any, 0);
                        int n;
                        try {
                            if(socket.Available==0) break;
                            n=socket.ReceiveFrom(buffer, ref source);
                        } catch(SocketException error) {
                            // Windows reports an ICMP port-unreachable from an
                            // earlier UDP send here while the game is starting.
                            if(error.SocketErrorCode==SocketError.ConnectionReset) continue;
                            throw;
                        }
                        if(side==0) client=source;
                        received++;
                        largest=Math.Max(largest,n);
                        if(n>ceiling) { oversized++; dropped++; continue; }
                        if(received % dropEvery == 0 || queue.Count >= 4096) { dropped++; continue; }
                        EndPoint target = side==0 ? server : client;
                        if(target==null) continue;
                        var data = new byte[n]; Array.Copy(buffer,data,n);
                        queue.Add(new Pending { Data=data, Target=target, Socket=side==0 ? back : front, Due=clock.ElapsedMilliseconds+delay+(jitter>0?random.Next(jitter+1):0) });
                    }
                }
                for(int index=0; index<queue.Count;) {
                    if(queue[index].Due>clock.ElapsedMilliseconds) { ++index; continue; }
                    var packet=queue[index]; queue.RemoveAt(index);
                    try { packet.Socket.SendTo(packet.Data,packet.Target); }
                    catch(SocketException error) { if(error.SocketErrorCode==SocketError.ConnectionReset) { dropped++; continue; } throw; }
                    forwarded++; bytes+=packet.Data.Length;
                    largestForwarded=Math.Max(largestForwarded,packet.Data.Length);
                }
                Thread.Sleep(1);
            }
            if(received==0 || dropped==0) throw new Exception("Relay did not exercise packet loss");
            return "received="+received+" forwarded="+forwarded+" dropped="+dropped+" forwardedBytes="+bytes+" oneWayDelayMs="+delay+" jitterMs="+jitter+" oversizedDropped="+oversized+" largestDatagram="+largest+" largestForwarded="+largestForwarded+" ceiling="+ceiling;
        }
    }
}
'@
$relayScript = Join-Path $run 'relay.ps1'
$sourceFile = Join-Path $run 'relay.cs'
$relaySource | Set-Content -LiteralPath $sourceFile -Encoding utf8
@'
param([string]$Source,[int]$Port,[int]$Delay,[int]$Jitter=0,[int]$DropEvery,[int]$Ceiling,[int]$Duration=75)
$ErrorActionPreference='Stop'
Add-Type -Path $Source
[CoopUdpRelay]::Run($Port,$Delay,$Jitter,$DropEvery,$Duration,$Ceiling)
'@ | Set-Content -LiteralPath $relayScript -Encoding utf8
$shellPath = (Get-Process -Id $PID).Path
$relay = Start-Process -FilePath $shellPath -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-File',$relayScript,'-Source',$sourceFile,'-Port',$Port,'-Delay',$OneWayDelayMs,'-Jitter',$JitterMs,'-DropEvery',$DropEvery,'-Ceiling',$MaxDatagramBytes,'-Duration',$(if($Effects){85}else{75})) -RedirectStandardOutput "$run/relay.log" -RedirectStandardError "$run/relay.err"
# Windows PowerShell reports a null ExitCode unless the handle was opened early.
$null = $relay.Handle
try {
    Start-Sleep -Seconds 2
    if($relay.HasExited) { throw 'UDP relay failed to start; inspect build/impaired-network/relay.err' }
    & "$PSScriptRoot/RunGameSmoke.ps1" -GameDirectory $GameDirectory -Executable $Executable -Port ($Port+1) -ClientPort $Port -Campaign:(!$MapOnly -and !$VisibilityProps) -Media:$Media -Prediction:$Prediction -SnapshotBlackout:$SnapshotBlackout -OnlineMenu:$OnlineMenu -VisibilityProps:$VisibilityProps -Effects:$Effects -Stairs:$Stairs -Gate:$Gate -Levers:$Levers -Flashback:$Flashback -LevelDoor:$LevelDoor -Spawn:$Spawn -Fairness:$Fairness -InputGap:$InputGap -StartMap $StartMap -MapOnly:($MapOnly -or $VisibilityProps)
    if (!$relay.WaitForExit(15000)) { throw 'UDP relay did not exit on its timer' }
    $relay.Refresh()
    if($relay.ExitCode -ne 0) { throw 'UDP relay failed; inspect relay.err' }
    Copy-Item -LiteralPath "$gameLogs/host.log" -Destination "$run/host.log"
    Copy-Item -LiteralPath "$gameLogs/client.log" -Destination "$run/client.log"
    Get-Content -LiteralPath "$run/relay.log"
    Write-Output 'PASS: campaign integration through delayed/lossy localhost UDP relay'
} finally {
    # Keep the counters even when the game assertions fail (e.g. an MTU
    # blackhole regression); the relay's own bounded timer releases sockets.
    if(!$relay.HasExited) { [void]$relay.WaitForExit(15000) }
    if(!$relay.HasExited) { Stop-Process -Id $relay.Id }
    foreach($role in @('host','client')) {
        $log = Join-Path $gameLogs "$role.log"
        if(Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $run "$role.log") }
    }
}
