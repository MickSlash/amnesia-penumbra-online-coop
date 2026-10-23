// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core of the OnlineCoop bridge: the local channel to the game (two XML
// files, see OnlineCoop_Core.hps) and the UDP link to the other player's
// bridge. Compiled into both CoopBridge.exe (command line, tests) and the
// launcher (which runs one Bridge on a background thread).
//
// Network model
//   - One UDP socket. The host listens; the client sends Hello until the host
//     answers Welcome (or Reject, e.g. different versions or a full session).
//   - State packets, 20 Hz each way, unreliable: the latest game state of the
//     sender as key/value pairs, plus timestamps for the round trip. A lost
//     state packet is simply superseded by the next one.
//   - Events, reliable: sequence numbers, cumulative acks, resend after
//     ~1.5 RTT, in-order delivery exactly once. For things that must never be
//     lost or doubled (inventory changes from phase 5 on).
//   - No packet for TimeoutMs -> peer lost. The client goes back to sending
//     Hello, so it reconnects by itself when the link comes back.
//
// Game state is forwarded generically: every attribute the game writes in
// game_out.xml (except channel bookkeeping) reaches the other game as
// "Peer<Name>" in its game_in.xml, so later phases add state without touching
// the bridge.
//
// Written for the C# 5 compiler that ships with .NET Framework 4.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Xml;

public enum BridgeRole { Host, Client }
public enum LinkState { Idle, Connecting, Connected, Rejected }

public sealed class BridgeOptions
{
    public BridgeRole Role = BridgeRole.Host;
    public string HostAddress = "";
    public int Port = Bridge.DefaultPort;
    public string PlayerName = Environment.UserName;
    public string IpcDir = Bridge.DefaultIpcDir;
    public string ModVersion = Bridge.ModVersion;
    // Network simulator, applied to every packet this bridge sends.
    public double SimLossPercent;
    public int SimDelayMs;
    public int SimJitterMs;
    public Action<string> Log = delegate { };
}

public sealed class Bridge
{
    public const int Protocol = 1;
    public const string ModVersion = "0.3.0";
    public const int DefaultPort = 27030;
    public const int StateIntervalMs = 50;
    public const int HelloIntervalMs = 500;
    public const int TimeoutMs = 8000;
    const uint Magic = 0x50434B42; // "BKCP"
    const byte PHello = 1, PWelcome = 2, PReject = 3, PState = 4, PEvent = 5, PAck = 6, PBye = 7;
    const int EventWindow = 64;

    public static readonly string DefaultIpcDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "My Games", "Amnesia The Bunker", "OnlineCoop");

    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);

    readonly BridgeOptions opt;
    readonly Stopwatch clock = Stopwatch.StartNew();
    readonly Random random = new Random();
    readonly object gate = new object();
    Socket socket;
    Thread thread;
    volatile bool stopping, silent;

    // ---- link state (network thread; snapshot fields read under gate) ----
    LinkState link = LinkState.Idle;
    EndPoint peer;
    uint session;
    long lastHeard, lastHello, lastStateSent;
    uint stateSeq, peerStateSeq;
    uint peerSendMs; long peerSendHeardAt = -1;
    double srtt = -1;
    string peerName = "", rejectReason = "";
    Dictionary<string, string> peerState = new Dictionary<string, string>();
    long statesReceived, statesLost;

    // ---- reliable events ----
    sealed class Outgoing { public uint Seq; public string Payload; public long SentAt = -1; public int Sends; }
    readonly List<Outgoing> outbox = new List<Outgoing>();
    readonly Queue<string> pendingEvents = new Queue<string>();
    readonly SortedDictionary<uint, string> early = new SortedDictionary<uint, string>();
    uint nextEventSeq = 1, deliveredUpTo;
    long eventResends;

    // ---- network simulator ----
    sealed class Delayed { public long Due; public byte[] Data; public EndPoint To; }
    readonly List<Delayed> delayed = new List<Delayed>();

    // ---- game channel ----
    readonly string gameOut, gameIn;
    DateTime gameStamp = DateTime.MinValue;
    int gameSeq, gameInSeq;
    string gameAppTime = "";
    long gameHeardAt = -1;
    Dictionary<string, string> localState = new Dictionary<string, string>();
    long gameMsgs, gameParseRetries;

    // Raised on the network thread for every event delivered (in order, once).
    public event Action<string> EventReceived;

    public Bridge(BridgeOptions options)
    {
        opt = options;
        Directory.CreateDirectory(opt.IpcDir);
        gameOut = Path.Combine(opt.IpcDir, "game_out.xml");
        gameIn = Path.Combine(opt.IpcDir, "game_in.xml");
        // Leftovers of an earlier session would read as a live game (or a
        // live bridge); the game rewrites its file within 50 ms anyway.
        TryDelete(gameIn);
        TryDelete(gameOut);
    }

    // ------------------------------------------------------------------
    // Public surface (any thread)

    public void Start()
    {
        timeBeginPeriod(1);
        socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        // Windows reports an ICMP "port unreachable" as a reset on the next
        // receive; for a client polling a host that is not up yet that is
        // normal, so turn it off (SIO_UDP_CONNRESET).
        socket.IOControl(-1744830452, new byte[] { 0 }, null);
        socket.Blocking = false;
        if (opt.Role == BridgeRole.Host)
        {
            socket.Bind(new IPEndPoint(IPAddress.Any, opt.Port));
            Log("hosting on UDP port " + opt.Port);
        }
        else
        {
            socket.Bind(new IPEndPoint(IPAddress.Any, 0));
            peer = Resolve(opt.HostAddress, opt.Port);
            Log("joining " + peer);
            link = LinkState.Connecting;
        }
        thread = new Thread(Run) { IsBackground = true, Name = "OnlineCoop bridge" };
        thread.Start();
    }

    // Test hook: dies like a crashed process or a pulled cable (no Bye).
    public void Kill() { silent = true; Stop(); }

    public void Stop()
    {
        if (thread == null) return;
        stopping = true;
        thread.Join(2000);
        thread = null;
        if (socket != null) socket.Close();
    }

    // Queues a reliable event for the peer. Events queued while no peer is
    // connected are sent once the link is up.
    public void SendEvent(string payload)
    {
        lock (gate) pendingEvents.Enqueue(payload);
    }

    public sealed class Status
    {
        public LinkState Link; public string PeerName, RejectReason; public int RttMs;
        public bool GameRunning; public string PeerMap; public long StatesReceived, StatesLost, EventResends;
        public int EventsInFlight;
    }

    public Status GetStatus()
    {
        lock (gate)
        {
            string map;
            peerState.TryGetValue("Map", out map);
            return new Status
            {
                Link = link, PeerName = peerName, RejectReason = rejectReason, RttMs = srtt < 0 ? -1 : (int)srtt,
                GameRunning = gameHeardAt >= 0 && clock.ElapsedMilliseconds - gameHeardAt < 3000,
                PeerMap = map ?? "", StatesReceived = statesReceived, StatesLost = statesLost,
                EventResends = eventResends, EventsInFlight = outbox.Count + pendingEvents.Count
            };
        }
    }

    // Latest value of an attribute from the local game, or of the peer's game.
    public string LocalValue(string key) { lock (gate) { string v; return localState.TryGetValue(key, out v) ? v : ""; } }
    public string PeerValue(string key) { lock (gate) { string v; return peerState.TryGetValue(key, out v) ? v : ""; } }

    public string Summary()
    {
        var s = GetStatus();
        return string.Format(CultureInfo.InvariantCulture,
            "link={0} peer='{1}' rtt_ms={2} game={3} game_msgs={4} parse_retries={5} states_in={6} states_lost={7} events_in_flight={8} resends={9} peer_map='{10}'",
            s.Link, s.PeerName, s.RttMs, s.GameRunning ? "on" : "off", Interlocked.Read(ref gameMsgs), Interlocked.Read(ref gameParseRetries),
            s.StatesReceived, s.StatesLost, s.EventsInFlight, s.EventResends, s.PeerMap);
    }

    // ------------------------------------------------------------------
    // Network thread

    void Run()
    {
        var buffer = new byte[2048];
        while (!stopping)
        {
            long now = clock.ElapsedMilliseconds;
            try
            {
                while (socket.Available > 0)
                {
                    EndPoint from = new IPEndPoint(IPAddress.Any, 0);
                    int n = socket.ReceiveFrom(buffer, ref from);
                    Handle(buffer, n, from, now);
                }
            }
            catch (SocketException e)
            {
                if (e.SocketErrorCode != SocketError.WouldBlock && e.SocketErrorCode != SocketError.ConnectionReset)
                    Log("receive error: " + e.SocketErrorCode);
            }
            catch (EndOfStreamException) { } // truncated packet: ignore
            catch (ObjectDisposedException) { break; }

            PollGame(now);
            Tick(now);
            FlushDelayed(now);
            Thread.Sleep(1);
        }
        if (link == LinkState.Connected && !silent) Send(Packet(PBye).ToArray(), true);
    }

    void Tick(long now)
    {
        if (link == LinkState.Connected && now - lastHeard > TimeoutMs)
        {
            Log("peer '" + peerName + "' lost (no packets for " + TimeoutMs / 1000 + " s)");
            DropPeer(opt.Role == BridgeRole.Client ? LinkState.Connecting : LinkState.Idle);
        }
        if (opt.Role == BridgeRole.Client && link != LinkState.Connected)
        {
            // After a reject, keep trying slowly: the host may update or free its slot.
            int interval = link == LinkState.Rejected ? 3000 : HelloIntervalMs;
            if (now - lastHello >= interval)
            {
                lastHello = now;
                var w = Packet(PHello);
                w.Write(opt.ModVersion); w.Write(opt.PlayerName);
                Send(w.ToArray(), false);
            }
            return;
        }
        if (link != LinkState.Connected) return;

        if (now - lastStateSent >= StateIntervalMs)
        {
            lastStateSent = now;
            SendState(now);
        }
        PumpEvents(now);
    }

    void SendState(long now)
    {
        var w = Packet(PState);
        w.Write(++stateSeq);
        w.Write((uint)now);
        // Echo of the peer's clock plus how long we held it: the peer gets the
        // round trip without the two clocks ever being compared.
        w.Write(peerSendMs);
        w.Write(peerSendHeardAt < 0 ? uint.MaxValue : (uint)(now - peerSendHeardAt));
        w.Write(deliveredUpTo);
        Dictionary<string, string> state;
        lock (gate) state = new Dictionary<string, string>(localState);
        state["Game"] = gameHeardAt >= 0 && now - gameHeardAt < 3000 ? "1" : "0";
        w.Write((ushort)state.Count);
        foreach (var kv in state) { w.Write(kv.Key); w.Write(kv.Value); }
        Send(w.ToArray(), false);
    }

    void PumpEvents(long now)
    {
        lock (gate)
        {
            while (pendingEvents.Count > 0 && outbox.Count < EventWindow)
                outbox.Add(new Outgoing { Seq = nextEventSeq++, Payload = pendingEvents.Dequeue() });
        }
        long resendAfter = Math.Max(100, (long)((srtt < 0 ? 200 : srtt) * 1.5));
        foreach (var o in outbox)
        {
            if (o.SentAt >= 0 && now - o.SentAt < resendAfter) continue;
            if (o.Sends > 0) eventResends++;
            o.SentAt = now; o.Sends++;
            var w = Packet(PEvent);
            w.Write(o.Seq); w.Write(o.Payload);
            Send(w.ToArray(), false);
        }
    }

    void Handle(byte[] data, int length, EndPoint from, long now)
    {
        var r = new BinaryReader(new MemoryStream(data, 0, length), Encoding.UTF8);
        if (length < 10 || r.ReadUInt32() != Magic) return;
        byte proto = r.ReadByte(), type = r.ReadByte();
        uint packetSession = r.ReadUInt32();

        if (type == PHello && opt.Role == BridgeRole.Host)
        {
            string version = r.ReadString(), name = r.ReadString();
            if (proto != Protocol || version != opt.ModVersion)
            {
                Reject(from, "versioni diverse: host " + opt.ModVersion + ", client " + version);
                return;
            }
            bool samePeer = peer != null && peer.Equals(from);
            if (link == LinkState.Connected && !samePeer)
            {
                Reject(from, "partita già occupata da " + peerName);
                return;
            }
            if (!(link == LinkState.Connected && samePeer))
            {
                ResetSession();
                peer = from; session = (uint)random.Next(1, int.MaxValue);
                lock (gate) { link = LinkState.Connected; peerName = name; rejectReason = ""; }
                lastHeard = now;
                Log("client '" + name + "' connected from " + from);
            }
            var w = Packet(PWelcome);
            w.Write(opt.PlayerName);
            Send(w.ToArray(), false);
            return;
        }
        if (proto != Protocol) return;

        if (type == PWelcome && opt.Role == BridgeRole.Client && from.Equals(peer))
        {
            if (link != LinkState.Connected)
            {
                ResetSession();
                session = packetSession;
                string name = r.ReadString();
                lock (gate) { link = LinkState.Connected; peerName = name; rejectReason = ""; }
                Log("connected to host '" + name + "'");
            }
            lastHeard = now;
            return;
        }
        if (type == PReject && opt.Role == BridgeRole.Client && from.Equals(peer))
        {
            string reason = r.ReadString();
            if (link != LinkState.Rejected || reason != rejectReason) Log("host refused: " + reason);
            lock (gate) { link = LinkState.Rejected; rejectReason = reason; }
            return;
        }

        // Everything else belongs to the current session only.
        if (link != LinkState.Connected || !from.Equals(peer) || packetSession != session) return;
        lastHeard = now;
        switch (type)
        {
            case PState: ReadState(r, now); break;
            case PEvent:
            {
                uint seq = r.ReadUInt32(); string payload = r.ReadString();
                if (seq > deliveredUpTo && seq <= deliveredUpTo + 4 * EventWindow) early[seq] = payload;
                string next;
                while (early.TryGetValue(deliveredUpTo + 1, out next))
                {
                    early.Remove(++deliveredUpTo);
                    var handler = EventReceived;
                    if (handler != null) handler(next);
                }
                // Ack every event, duplicates included: the first ack may be lost.
                var w = Packet(PAck); w.Write(deliveredUpTo);
                Send(w.ToArray(), false);
                break;
            }
            case PAck: Acked(r.ReadUInt32()); break;
            case PBye:
                Log("peer '" + peerName + "' left");
                DropPeer(opt.Role == BridgeRole.Client ? LinkState.Connecting : LinkState.Idle);
                break;
        }
    }

    void ReadState(BinaryReader r, long now)
    {
        uint seq = r.ReadUInt32(), sendMs = r.ReadUInt32(), echoMs = r.ReadUInt32(), hold = r.ReadUInt32(), ack = r.ReadUInt32();
        int count = r.ReadUInt16();
        var state = new Dictionary<string, string>();
        for (int i = 0; i < count; i++) { string k = r.ReadString(); state[k] = r.ReadString(); }
        Acked(ack);
        if (seq <= peerStateSeq) return; // late or duplicated: a newer one is already applied
        if (peerStateSeq != 0) statesLost += seq - peerStateSeq - 1;
        peerStateSeq = seq;
        peerSendMs = sendMs; peerSendHeardAt = now;
        if (hold != uint.MaxValue && echoMs != 0)
        {
            double sample = (uint)now - echoMs - hold;
            if (sample >= 0 && sample < 10000) srtt = srtt < 0 ? sample : srtt * 0.875 + sample * 0.125;
        }
        lock (gate) { peerState = state; statesReceived++; }
    }

    void Acked(uint upTo)
    {
        outbox.RemoveAll(o => o.Seq <= upTo);
    }

    void ResetSession()
    {
        // A new session starts from scratch: the game resynchronises from the
        // host's state, so events of a dead session are not replayed.
        if (outbox.Count > 0) Log(outbox.Count + " unacknowledged events of the previous session dropped");
        outbox.Clear(); early.Clear();
        nextEventSeq = 1; deliveredUpTo = 0;
        stateSeq = 0; peerStateSeq = 0; peerSendMs = 0; peerSendHeardAt = -1; srtt = -1;
        lock (gate) { peerState = new Dictionary<string, string>(); }
    }

    void DropPeer(LinkState next)
    {
        lock (gate) { link = next; peerState = new Dictionary<string, string>(); }
        if (opt.Role == BridgeRole.Host) peer = null;
        session = 0;
    }

    void Reject(EndPoint to, string reason)
    {
        var w = Packet(PReject); w.Write(reason);
        var data = w.ToArray();
        SendTo(data, to, false);
    }

    // ------------------------------------------------------------------
    // Packets and the network simulator

    sealed class PacketWriter : BinaryWriter
    {
        public PacketWriter() : base(new MemoryStream(), Encoding.UTF8) { }
        public byte[] ToArray() { Flush(); return ((MemoryStream)BaseStream).ToArray(); }
    }

    PacketWriter Packet(byte type)
    {
        var w = new PacketWriter();
        w.Write(Magic); w.Write((byte)Protocol); w.Write(type); w.Write(session);
        return w;
    }

    void Send(byte[] data, bool immediate) { if (peer != null) SendTo(data, peer, immediate); }

    void SendTo(byte[] data, EndPoint to, bool immediate)
    {
        if (!immediate)
        {
            if (opt.SimLossPercent > 0 && random.NextDouble() * 100 < opt.SimLossPercent) return;
            if (opt.SimDelayMs > 0 || opt.SimJitterMs > 0)
            {
                long due = clock.ElapsedMilliseconds + opt.SimDelayMs + (opt.SimJitterMs > 0 ? random.Next(opt.SimJitterMs + 1) : 0);
                delayed.Add(new Delayed { Due = due, Data = data, To = to });
                return;
            }
        }
        try { socket.SendTo(data, to); }
        catch (SocketException e) { if (e.SocketErrorCode != SocketError.WouldBlock) Log("send error: " + e.SocketErrorCode); }
    }

    void FlushDelayed(long now)
    {
        for (int i = 0; i < delayed.Count; )
        {
            if (delayed[i].Due > now) { i++; continue; }
            var d = delayed[i]; delayed.RemoveAt(i);
            try { socket.SendTo(d.Data, d.To); } catch (SocketException) { }
        }
    }

    // ------------------------------------------------------------------
    // Game channel: game_out.xml in, game_in.xml out (one write per game
    // heartbeat, so the game never sees more files than it produces).

    static readonly HashSet<string> Bookkeeping = new HashSet<string> { "Proto", "Seq", "AppTime", "LastInSeq" };

    void PollGame(long now)
    {
        DateTime stamp;
        try { stamp = File.GetLastWriteTimeUtc(gameOut); } catch { return; }
        if (stamp == gameStamp || stamp.Year < 2000) return;
        XmlElement root = ReadRoot(gameOut);
        if (root == null) { Interlocked.Increment(ref gameParseRetries); return; } // mid-write: next poll
        gameStamp = stamp;
        int seq;
        if (root.GetAttribute("Proto") != "1" || !int.TryParse(root.GetAttribute("Seq"), out seq) || seq == gameSeq) return;
        gameSeq = seq;
        gameAppTime = root.GetAttribute("AppTime");
        Interlocked.Increment(ref gameMsgs);
        var state = new Dictionary<string, string>();
        foreach (XmlAttribute a in root.Attributes) if (!Bookkeeping.Contains(a.Name)) state[a.Name] = a.Value;
        lock (gate) { localState = state; gameHeardAt = now; }
        WriteGameIn();
    }

    void WriteGameIn()
    {
        var sb = new StringBuilder("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CoopIpc");
        Attr(sb, "Proto", "1");
        Attr(sb, "Seq", (++gameInSeq).ToString(CultureInfo.InvariantCulture));
        Attr(sb, "EchoSeq", gameSeq.ToString(CultureInfo.InvariantCulture));
        Attr(sb, "EchoAppTime", gameAppTime);
        lock (gate)
        {
            Attr(sb, "Role", opt.Role.ToString());
            Attr(sb, "Net", link.ToString());
            Attr(sb, "NetRtt", srtt < 0 ? "-1" : ((int)srtt).ToString(CultureInfo.InvariantCulture));
            Attr(sb, "PeerName", peerName);
            if (link == LinkState.Rejected) Attr(sb, "NetError", rejectReason);
            foreach (var kv in peerState) Attr(sb, "Peer" + kv.Key, kv.Value);
        }
        sb.Append(" />\n");
        WriteAtomic(gameIn, sb.ToString());
    }

    static void Attr(StringBuilder sb, string name, string value)
    {
        sb.Append(' ').Append(name).Append("=\"").Append(SecurityEscape(value ?? "")).Append('"');
    }

    static string SecurityEscape(string s) { return System.Security.SecurityElement.Escape(s); }

    // Opens with full sharing so the game is never blocked from rewriting the
    // file while it is being read; returns null for a partial document.
    public static XmlElement ReadRoot(string path)
    {
        try
        {
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                var doc = new XmlDocument();
                doc.Load(fs);
                return doc.DocumentElement;
            }
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
        catch (XmlException) { return null; }
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool MoveFileEx(string from, string to, uint flags);

    // Temp file + rename, retried while the game holds the target open.
    public static bool WriteAtomic(string path, string text)
    {
        string tmp = path + ".tmp";
        File.WriteAllText(tmp, text, new UTF8Encoding(false));
        for (int retry = 0; retry < 50; retry++)
        {
            if (MoveFileEx(tmp, path, 1 /*REPLACE_EXISTING*/ | 8 /*WRITE_THROUGH*/)) return true;
            Thread.Sleep(1);
        }
        return false;
    }

    // ------------------------------------------------------------------

    static IPEndPoint Resolve(string address, int defaultPort)
    {
        string host = address.Trim(); int port = defaultPort;
        int colon = host.LastIndexOf(':');
        if (colon > 0 && host.IndexOf(':') == colon) { port = int.Parse(host.Substring(colon + 1), CultureInfo.InvariantCulture); host = host.Substring(0, colon); }
        IPAddress ip;
        if (!IPAddress.TryParse(host, out ip))
        {
            ip = null;
            foreach (var a in Dns.GetHostAddresses(host)) if (a.AddressFamily == AddressFamily.InterNetwork) { ip = a; break; }
            if (ip == null) throw new ArgumentException("indirizzo non valido: " + address);
        }
        return new IPEndPoint(ip, port);
    }

    static void TryDelete(string path) { try { File.Delete(path); } catch { } }

    void Log(string text) { opt.Log(text); }
}
