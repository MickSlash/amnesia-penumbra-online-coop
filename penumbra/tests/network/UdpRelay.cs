using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;
public static class CoopUdpRelay {
    class Pending { public byte[] Data; public EndPoint Target; public Socket Socket; public long Due; }
    public static string Run(int port, int delay, int dropEvery, int seconds, int ceiling, string stopFile) {
        using(var front = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
        using(var back = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp)) {
            front.Bind(new IPEndPoint(IPAddress.Loopback, port));
            back.Bind(new IPEndPoint(IPAddress.Loopback, 0));
            EndPoint server = new IPEndPoint(IPAddress.Loopback, port+1), client = null;
            var queue = new Queue<Pending>(); var clock = Stopwatch.StartNew();
            var buffer = new byte[65536]; long received = 0, dropped = 0, forwarded = 0, bytes = 0;
            long oversized = 0; int largest = 0, largestForwarded = 0;
            while(clock.ElapsedMilliseconds < seconds*1000L && (String.IsNullOrEmpty(stopFile) || !System.IO.File.Exists(stopFile))) {
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
                        queue.Enqueue(new Pending { Data=data, Target=target, Socket=side==0 ? back : front, Due=clock.ElapsedMilliseconds+delay });
                    }
                }
                while(queue.Count>0 && queue.Peek().Due<=clock.ElapsedMilliseconds) {
                    var packet=queue.Dequeue();
                    try { packet.Socket.SendTo(packet.Data,packet.Target); }
                    catch(SocketException error) { if(error.SocketErrorCode==SocketError.ConnectionReset) { dropped++; continue; } throw; }
                    forwarded++; bytes+=packet.Data.Length;
                    largestForwarded=Math.Max(largestForwarded,packet.Data.Length);
                }
                Thread.Sleep(1);
            }
            if(received==0 || dropped==0) throw new Exception("Relay did not exercise packet loss");
            return "received="+received+" forwarded="+forwarded+" dropped="+dropped+" forwardedBytes="+bytes+" oneWayDelayMs="+delay+" oversizedDropped="+oversized+" largestDatagram="+largest+" largestForwarded="+largestForwarded+" ceiling="+ceiling;
        }
    }
}