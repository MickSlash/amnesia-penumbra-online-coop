# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [ValidateRange(1,65534)][int]$Port = 27036,
    [ValidateRange(0,500)][int]$OneWayDelayMs = 50,
    [ValidateRange(2,1000)][int]$DropEvery = 20,
    [switch]$Media
)
$ErrorActionPreference = 'Stop'
$run = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path 'build/impaired-network'
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
    public static string Run(int port, int delay, int dropEvery, int seconds) {
        using(var front = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
        using(var back = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp)) {
            front.Bind(new IPEndPoint(IPAddress.Loopback, port));
            back.Bind(new IPEndPoint(IPAddress.Loopback, 0));
            EndPoint server = new IPEndPoint(IPAddress.Loopback, port+1), client = null;
            var queue = new Queue<Pending>(); var clock = Stopwatch.StartNew();
            var buffer = new byte[65536]; long received = 0, dropped = 0, forwarded = 0, bytes = 0;
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
                        if(received % dropEvery == 0 || queue.Count >= 4096) { dropped++; continue; }
                        EndPoint target = side==0 ? server : client;
                        if(target==null) continue;
                        var data = new byte[n]; Array.Copy(buffer,data,n);
                        queue.Enqueue(new Pending { Data=data, Target=target, Socket=side==0 ? back : front, Due=clock.ElapsedMilliseconds+delay });
                    }
                }
                while(queue.Count>0 && queue.Peek().Due<=clock.ElapsedMilliseconds) {
                    var packet=queue.Dequeue();
                    try { packet.Socket.SendTo(packet.Data,packet.Target); }
                    catch(SocketException error) { if(error.SocketErrorCode==SocketError.ConnectionReset) { dropped++; continue; } throw; }
                    forwarded++; bytes+=packet.Data.Length;
                }
                Thread.Sleep(1);
            }
            if(received==0 || dropped==0) throw new Exception("Relay did not exercise packet loss");
            return "received="+received+" forwarded="+forwarded+" dropped="+dropped+" forwardedBytes="+bytes+" oneWayDelayMs="+delay;
        }
    }
}
'@
$relayScript = Join-Path $run 'relay.ps1'
$sourceFile = Join-Path $run 'relay.cs'
$relaySource | Set-Content -LiteralPath $sourceFile -Encoding utf8
@'
param([string]$Source,[int]$Port,[int]$Delay,[int]$DropEvery)
$ErrorActionPreference='Stop'
Add-Type -Path $Source
[CoopUdpRelay]::Run($Port,$Delay,$DropEvery,75)
'@ | Set-Content -LiteralPath $relayScript -Encoding utf8
$shellPath = (Get-Process -Id $PID).Path
$relay = Start-Process -FilePath $shellPath -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-File',$relayScript,'-Source',$sourceFile,'-Port',$Port,'-Delay',$OneWayDelayMs,'-DropEvery',$DropEvery) -RedirectStandardOutput "$run/relay.log" -RedirectStandardError "$run/relay.err"
try {
    Start-Sleep -Seconds 2
    if($relay.HasExited) { throw 'UDP relay failed to start; inspect build/impaired-network/relay.err' }
    & "$PSScriptRoot/RunGameSmoke.ps1" -GameDirectory $GameDirectory -Port ($Port+1) -ClientPort $Port -Campaign -Media:$Media
    if (!$relay.WaitForExit(15000)) { throw 'UDP relay did not exit on its timer' }
    $relay.Refresh()
    if($relay.ExitCode -ne 0) { throw 'UDP relay failed; inspect relay.err' }
    Copy-Item -LiteralPath "$PSScriptRoot/../../build/player-replication/host.log" -Destination "$run/host.log"
    Copy-Item -LiteralPath "$PSScriptRoot/../../build/player-replication/client.log" -Destination "$run/client.log"
    Get-Content -LiteralPath "$run/relay.log"
    Write-Output 'PASS: campaign integration through delayed/lossy localhost UDP relay'
} finally {
    if(!$relay.HasExited) { Stop-Process -Id $relay.Id }
    foreach($role in @('host','client')) {
        $log = Join-Path "$PSScriptRoot/../../build/player-replication" "$role.log"
        if(Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $run "$role.log") }
    }
}
