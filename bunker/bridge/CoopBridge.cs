// SPDX-License-Identifier: GPL-3.0-or-later
//
// Command-line front end of the OnlineCoop bridge (the launcher embeds the
// same BridgeCore). Players normally use the launcher; this is for testing
// and for running a bridge by hand.
//
//   CoopBridge --host [--port 27030]                  host a session
//   CoopBridge --join 26.1.2.3[:27030]                join a host
//     options: --name NAME  --ipc-dir DIR  --minutes N
//              --sim-loss PCT  --sim-delay MS  --sim-jitter MS   (network simulator)
//              --fake-game     a stand-in game on the ipc folder (no game needed)
//              --fake-walker   a stand-in that walks/runs a loop in front of the real player
//   CoopBridge --test-net [--sim-loss 5 --sim-delay 60 --sim-jitter 30]
//              host + client + two stand-in games in this process, scripted checks
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using System.Xml;

static class CoopBridge
{
    static volatile bool stop;

    static int Main(string[] args)
    {
        var opt = new BridgeOptions { Log = text => Log("BRIDGE", text) };
        double minutes = 0; bool fakeGame = false, fakeWalker = false, testNet = false; string mode = null;
        try
        {
            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i];
                Func<string> next = () => { if (i + 1 >= args.Length) throw new ArgumentException(a + " needs a value"); return args[++i]; };
                switch (a)
                {
                    case "--host": mode = "host"; break;
                    case "--join": mode = "join"; opt.HostAddress = next(); break;
                    case "--port": opt.Port = int.Parse(next(), CultureInfo.InvariantCulture); break;
                    case "--name": opt.PlayerName = next(); break;
                    case "--ipc-dir": opt.IpcDir = next(); break;
                    case "--minutes": minutes = double.Parse(next(), CultureInfo.InvariantCulture); break;
                    case "--sim-loss": opt.SimLossPercent = double.Parse(next(), CultureInfo.InvariantCulture); break;
                    case "--sim-delay": opt.SimDelayMs = int.Parse(next(), CultureInfo.InvariantCulture); break;
                    case "--sim-jitter": opt.SimJitterMs = int.Parse(next(), CultureInfo.InvariantCulture); break;
                    case "--fake-game": fakeGame = true; break;
                    case "--fake-walker": fakeGame = true; fakeWalker = true; break;
                    case "--test-net": testNet = true; break;
                    default: throw new ArgumentException("unknown option " + a);
                }
            }
            if (testNet) return NetTest.Run(opt) ? 0 : 1;
            if (mode == null) throw new ArgumentException("--host or --join is required");
        }
        catch (Exception e)
        {
            if (!(e is ArgumentException || e is FormatException)) throw;
            Console.Error.WriteLine(e.Message);
            Console.Error.WriteLine("usage: CoopBridge --host | --join ADDRESS [--port N] [--name NAME] [--ipc-dir DIR] [--minutes N] [--fake-game] [--sim-loss PCT --sim-delay MS --sim-jitter MS] | --test-net");
            return 2;
        }

        opt.Role = mode == "host" ? BridgeRole.Host : BridgeRole.Client;
        var bridge = new Bridge(opt);
        bridge.EventReceived += payload => Log("BRIDGE", "event: " + payload);
        bridge.Start();
        FakeGame fake = null;
        if (fakeGame) { fake = new FakeGame(opt.IpcDir, opt.Role == BridgeRole.Host ? 0 : 100) { Walker = fakeWalker }; fake.Start(); }
        Console.CancelKeyPress += (s, e) => { e.Cancel = true; stop = true; };
        var clock = Stopwatch.StartNew();
        long deadline = minutes > 0 ? (long)(minutes * 60000) : long.MaxValue, nextStats = 10000;
        while (!stop && clock.ElapsedMilliseconds < deadline)
        {
            Thread.Sleep(100);
            if (clock.ElapsedMilliseconds >= nextStats)
            {
                nextStats += 10000;
                Log("BRIDGE", bridge.Summary() + (fake != null ? " | fake game: " + fake.Summary() : ""));
                if (bridge.LocalValue("Map") != "")
                    Log("BRIDGE", "local game: map='" + bridge.LocalValue("Map") + "' feet=" + bridge.LocalValue("PlayerFeet")
                        + " yaw=" + bridge.LocalValue("PlayerYaw") + " avatar=" + bridge.LocalValue("Avatar")
                        + " | peer feet=" + bridge.PeerValue("PlayerFeet"));
            }
        }
        if (fake != null) fake.Stop();
        bridge.Stop();
        return 0;
    }

    public static void Log(string who, string text)
    {
        lock (typeof(CoopBridge)) Console.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + " [" + who + "] " + text);
    }
}

// Stand-in for the game: same files, same cadence (50 ms writes, reads every
// other 60 Hz frame) and the same in-place, non-atomic write as the script API.
sealed class FakeGame
{
    readonly string outFile, inFile;
    readonly double baseX;
    Thread thread;
    volatile bool stop;
    readonly object gate = new object();
    int sent, echoes, rttMax; long rttSum;
    public Dictionary<string, string> LastIn = new Dictionary<string, string>();

    public FakeGame(string ipcDir, double x)
    {
        Directory.CreateDirectory(ipcDir);
        outFile = Path.Combine(ipcDir, "game_out.xml"); inFile = Path.Combine(ipcDir, "game_in.xml"); baseX = x;
    }

    public void Start() { thread = new Thread(Run) { IsBackground = true }; thread.Start(); }
    public void Stop() { stop = true; if (thread != null) thread.Join(); }

    void Run()
    {
        var clock = Stopwatch.StartNew();
        int seq = 0, lastIn = 0; long nextWrite = 0;
        while (!stop)
        {
            long now = clock.ElapsedMilliseconds;
            if (now >= nextWrite)
            {
                nextWrite = now + 50;
                string body;
                if (Walker) body = WalkerState(now);
                else
                {
                    string pos = string.Format(CultureInfo.InvariantCulture, "{0:0.00} 1 {1:0.00}", baseX + now / 1000.0, 5.0);
                    body = "Map=\"fake_map\" PlayerPos=\"" + pos + "\"";
                }
                string text = string.Format(CultureInfo.InvariantCulture,
                    "<CoopIpc Proto=\"1\" Seq=\"{0}\" AppTime=\"{1}\" LastInSeq=\"{2}\" {3} />\n", ++seq, now, lastIn, body);
                try
                {
                    using (var fs = new FileStream(outFile, FileMode.Create, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
                    using (var w = new StreamWriter(fs)) w.Write(text);
                    lock (gate) sent++;
                }
                catch (IOException) { }
            }
            XmlElement root = File.Exists(inFile) ? Bridge.ReadRoot(inFile) : null;
            int inSeq;
            if (root != null && int.TryParse(root.GetAttribute("Seq"), out inSeq) && inSeq > lastIn)
            {
                lastIn = inSeq;
                var values = new Dictionary<string, string>();
                foreach (XmlAttribute a in root.Attributes) values[a.Name] = a.Value;
                int rtt = (int)(clock.ElapsedMilliseconds - long.Parse(root.GetAttribute("EchoAppTime"), CultureInfo.InvariantCulture));
                lock (gate) { LastIn = values; echoes++; rttSum += rtt; if (rtt > rttMax) rttMax = rtt; }
            }
            Thread.Sleep(33);
        }
    }

    public string Get(string key) { lock (gate) { string v; return LastIn.TryGetValue(key, out v) ? v : ""; } }

    // --fake-walker: a second player that joins the real player's map and
    // walks a fixed loop in front of where the real player looked when first
    // seen: idle 3 s facing the player, walk 5.5 m away at 1.4 m/s, idle 2 s,
    // run back at 4.5 m/s. For checking the avatar's animations by eye.
    public bool Walker;
    string walkMap = "";
    double cx, cz, cy, fx, fz;
    long cycleStart;

    string WalkerState(long now)
    {
        string map = Get("PeerMap"), feet = Get("PeerPlayerFeet"), yaw = Get("PeerPlayerYaw");
        if (map != "" && map != "main_menu" && map != walkMap && feet != "" && feet != "0 0 0")
        {
            var p = feet.Split(' ');
            double y = double.Parse(yaw, CultureInfo.InvariantCulture);
            cx = double.Parse(p[0], CultureInfo.InvariantCulture); cy = double.Parse(p[1], CultureInfo.InvariantCulture);
            cz = double.Parse(p[2], CultureInfo.InvariantCulture);
            // Camera forward for a yaw (verified in game): (-sin, 0, -cos).
            fx = -Math.Sin(y); fz = -Math.Cos(y);
            walkMap = map; cycleStart = now;
            CoopBridge.Log("FAKE", string.Format(CultureInfo.InvariantCulture, "walking in '{0}' in front of {1:0.0} {2:0.0} {3:0.0}", map, cx, cy, cz));
        }
        if (walkMap == "") return "Map=\"\"";

        const double Near = 2.5, Far = 8.0, Walk = 1.4, Run = 4.5;
        double tWalk = (Far - Near) / Walk, tRun = (Far - Near) / Run;
        double t = ((now - cycleStart) / 1000.0) % (3 + tWalk + 2 + tRun), d; string phase; bool away;
        if (t < 3) { d = Near; phase = "idle"; away = false; }
        else if (t < 3 + tWalk) { d = Near + (t - 3) * Walk; phase = "walk"; away = true; }
        else if (t < 5 + tWalk) { d = Far; phase = "idle"; away = true; }
        else { d = Far - (t - 5 - tWalk) * Run; phase = "run"; away = false; }
        // Face the direction of travel; idle at the near end faces the player.
        double dirX = away ? fx : -fx, dirZ = away ? fz : -fz;
        double faceYaw = Math.Atan2(-dirX, -dirZ);
        lock (gate) walkPhase = phase;
        return string.Format(CultureInfo.InvariantCulture, "Map=\"{0}\" PlayerFeet=\"{1:0.000} {2:0.000} {3:0.000}\" PlayerPos=\"{1:0.000} {4:0.000} {3:0.000}\" PlayerYaw=\"{5:0.0000}\"",
            walkMap, cx + fx * d, cy, cz + fz * d, cy + 0.9, faceYaw);
    }
    string walkPhase = "";
    public string WalkPhase { get { lock (gate) return walkPhase; } }

    public int Sent { get { lock (gate) return sent; } }
    public int Echoes { get { lock (gate) return echoes; } }

    public string Summary()
    {
        lock (gate) return string.Format(CultureInfo.InvariantCulture, "sent={0} echoes={1} local_rtt_ms avg/max={2}/{3} net={4} peer_pos='{5}'",
            sent, echoes, echoes > 0 ? rttSum / echoes : 0, rttMax, Get2("Net"), Get2("PeerPlayerPos"));
    }
    string Get2(string key) { string v; return LastIn.TryGetValue(key, out v) ? v : ""; }
}

// Scripted end-to-end checks of the network layer, all in one process on
// localhost. Network impairment comes from the bridges' simulator options.
static class NetTest
{
    static int failures;

    static void Check(bool ok, string what)
    {
        CoopBridge.Log("TEST", (ok ? "PASS " : "FAIL ") + what);
        if (!ok) failures++;
    }

    static bool WaitFor(Func<bool> condition, int timeoutMs)
    {
        var watch = Stopwatch.StartNew();
        while (watch.ElapsedMilliseconds < timeoutMs) { if (condition()) return true; Thread.Sleep(20); }
        return condition();
    }

    static Bridge Make(BridgeOptions template, BridgeRole role, string dir, string name, string version = null)
    {
        var o = new BridgeOptions
        {
            Role = role, HostAddress = "127.0.0.1:" + template.Port, Port = template.Port, PlayerName = name, IpcDir = dir,
            SimLossPercent = template.SimLossPercent, SimDelayMs = template.SimDelayMs, SimJitterMs = template.SimJitterMs,
            ModVersion = version ?? Bridge.ModVersion, Log = text => CoopBridge.Log(name, text)
        };
        return new Bridge(o);
    }

    public static bool Run(BridgeOptions template)
    {
        string root = Path.Combine(Path.GetTempPath(), "OnlineCoopNetTest");
        string dirHost = Path.Combine(root, "host"), dirClient = Path.Combine(root, "client"), dirOther = Path.Combine(root, "other");
        CoopBridge.Log("TEST", string.Format(CultureInfo.InvariantCulture, "simulator: loss {0}% delay {1} ms jitter {2} ms",
            template.SimLossPercent, template.SimDelayMs, template.SimJitterMs));

        var host = Make(template, BridgeRole.Host, dirHost, "host");
        var client = Make(template, BridgeRole.Client, dirClient, "client");
        var gameHost = new FakeGame(dirHost, 0); var gameClient = new FakeGame(dirClient, 100);
        var fromClient = new List<string>(); var fromHost = new List<string>();
        host.EventReceived += p => { lock (fromClient) fromClient.Add(p); };
        client.EventReceived += p => { lock (fromHost) fromHost.Add(p); };
        host.Start(); client.Start(); gameHost.Start(); gameClient.Start();

        // 1. Handshake.
        Check(WaitFor(() => host.GetStatus().Link == LinkState.Connected && client.GetStatus().Link == LinkState.Connected, 5000),
            "handshake: both bridges connected within 5 s");

        // 2. Game state both ways (stand-in games are 100 m apart on X).
        Check(WaitFor(() => X(gameHost.Get("PeerPlayerPos")) >= 100 && X(gameClient.Get("PeerPlayerPos")) < 100, 5000),
            "state: each game sees the other's position (host sees '" + gameHost.Get("PeerPlayerPos") + "', client sees '" + gameClient.Get("PeerPlayerPos") + "')");
        Check(gameHost.Get("Net") == "Connected" && gameClient.Get("PeerName") == "host" && gameHost.Get("PeerName") == "client",
            "state: games see link state and peer names");
        Thread.Sleep(2000);
        int rtt = host.GetStatus().RttMs, expected = 2 * template.SimDelayMs + template.SimJitterMs;
        Check(rtt >= 0 && rtt <= expected + 60, "rtt: " + rtt + " ms (expected about " + expected + " ms)");

        // 3. Reliable events, both directions at once.
        const int N = 300;
        for (int i = 0; i < N; i++) { host.SendEvent("h" + i); client.SendEvent("c" + i); }
        bool delivered = WaitFor(() => { lock (fromClient) lock (fromHost) return fromClient.Count >= N && fromHost.Count >= N; }, 30000);
        Thread.Sleep(500); // late duplicates, if any, would show up now
        Check(delivered && InOrderOnce(fromClient, "c", N) && InOrderOnce(fromHost, "h", N),
            "events: " + N + " each way, delivered once and in order (got " + fromClient.Count + " / " + fromHost.Count
            + ", resends " + host.GetStatus().EventResends + " / " + client.GetStatus().EventResends + ")");

        // 4. A second client while the session is taken.
        var intruder = Make(template, BridgeRole.Client, dirOther, "intruder");
        intruder.Start();
        Check(WaitFor(() => intruder.GetStatus().Link == LinkState.Rejected, 5000) && host.GetStatus().PeerName == "client",
            "full session: second client refused ('" + intruder.GetStatus().RejectReason + "'), first one kept");
        intruder.Stop();

        // 5. Client dies without a goodbye: host notices within the timeout.
        var watch = Stopwatch.StartNew();
        client.Kill();
        bool lost = WaitFor(() => host.GetStatus().Link != LinkState.Connected, Bridge.TimeoutMs + 2000);
        Check(lost && watch.ElapsedMilliseconds >= Bridge.TimeoutMs - 500,
            "timeout: host dropped the dead client after " + watch.ElapsedMilliseconds + " ms");
        WaitFor(() => gameHost.Get("Net") != "Connected", 2000);
        Check(gameHost.Get("Net") != "Connected" && gameHost.Get("PeerPlayerPos") == "", "timeout: host game told the peer is gone");

        // 6. The client comes back.
        var client2 = Make(template, BridgeRole.Client, dirClient, "client");
        client2.Start();
        Check(WaitFor(() => host.GetStatus().Link == LinkState.Connected && client2.GetStatus().Link == LinkState.Connected, 5000)
            && WaitFor(() => X(gameHost.Get("PeerPlayerPos")) >= 100, 3000),
            "reconnect: client back, state flowing again");
        client2.Stop();
        Check(WaitFor(() => host.GetStatus().Link != LinkState.Connected, 2000), "goodbye: clean exit seen at once by the host");

        // 7. Version mismatch is refused with a readable reason.
        var old = Make(template, BridgeRole.Client, dirOther, "old", "0.0.1");
        old.Start();
        Check(WaitFor(() => old.GetStatus().Link == LinkState.Rejected, 5000) && old.GetStatus().RejectReason.Contains("versioni"),
            "version: mismatched client refused ('" + old.GetStatus().RejectReason + "')");
        old.Stop();

        gameHost.Stop(); gameClient.Stop(); host.Stop();
        CoopBridge.Log("TEST", failures == 0 ? "ALL PASSED" : failures + " CHECK(S) FAILED");
        return failures == 0;
    }

    // X coordinate of a "x y z" position; NaN (fails every comparison) if absent.
    static double X(string pos)
    {
        double x;
        if (string.IsNullOrEmpty(pos)) return double.NaN;
        return double.TryParse(pos.Split(' ')[0], NumberStyles.Float, CultureInfo.InvariantCulture, out x) ? x : double.NaN;
    }

    static bool InOrderOnce(List<string> got, string prefix, int n)
    {
        lock (got)
        {
            if (got.Count != n) return false;
            for (int i = 0; i < n; i++) if (got[i] != prefix + i) return false;
            return true;
        }
    }
}
