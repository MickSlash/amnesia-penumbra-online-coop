// SPDX-License-Identifier: GPL-3.0-or-later
//
// One-click launcher for the Amnesia: The Bunker online co-op mod:
//   1. host or join (address of the other player), player name;
//   2. installs / updates the OnlineCoop add-on in the game's mods folder;
//   3. runs the bridge (BridgeCore.cs) inside this process;
//   4. starts the game with the add-on enabled, exactly as the game's own
//      Mods menu relaunches itself;
//   5. shows the link state until the game closes, then ends the session.
// Settings live in %LOCALAPPDATA%\BunkerOnlineCoop. Written for C# 5.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

// Artwork embedded by tools/BuildLauncher.ps1; a missing resource never blocks play.
static class LauncherArt
{
    static Stream Open(string name) { return typeof(LauncherArt).Assembly.GetManifestResourceStream(name); }
    public static Icon WindowIcon() { var s = Open("launcher.ico"); return s == null ? null : new Icon(s); }
    public static Image Splash()
    {
        var stream = Open("launcher_splash.png");
        // Image.FromStream reads lazily: copy into a bitmap that owns its pixels.
        if (stream == null) return null;
        using (stream) using (var image = Image.FromStream(stream)) return new Bitmap(image);
    }
}

// Borderless loading screen shown while the game starts.
sealed class LauncherSplash : Form
{
    readonly Image art = LauncherArt.Splash();
    string status;
    public LauncherSplash(string text)
    {
        status = text;
        FormBorderStyle = FormBorderStyle.None; StartPosition = FormStartPosition.CenterScreen;
        ShowInTaskbar = true; Text = BunkerLauncher.Title; Icon = LauncherArt.WindowIcon();
        BackColor = Color.Black; DoubleBuffered = true;
        ClientSize = new Size(420, 420);
        // The artwork is a rounded tile: clip the window to the same shape.
        using (var shape = new GraphicsPath())
        {
            int d = (int)(ClientSize.Width * 0.38f), w = ClientSize.Width, h = ClientSize.Height;
            shape.AddArc(0, 0, d, d, 180, 90); shape.AddArc(w - d, 0, d, d, 270, 90);
            shape.AddArc(w - d, h - d, d, d, 0, 90); shape.AddArc(0, h - d, d, d, 90, 90);
            shape.CloseFigure();
            Region = new Region(shape);
        }
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        if (art != null) g.DrawImage(art, ClientRectangle);
        var band = new Rectangle(0, ClientSize.Height - 74, ClientSize.Width, 40);
        using (var shade = new SolidBrush(Color.FromArgb(170, 0, 0, 0))) g.FillRectangle(shade, band);
        using (var font = new Font("Segoe UI", 11, FontStyle.Bold))
        using (var format = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
            g.DrawString(status, font, Brushes.Gainsboro, band, format);
    }
    protected override void Dispose(bool disposing) { if (disposing && art != null) art.Dispose(); base.Dispose(disposing); }
    // Pump messages for a bounded time so the splash keeps painting.
    public static void Wait(int milliseconds, Func<bool> done)
    {
        var watch = Stopwatch.StartNew();
        while (watch.ElapsedMilliseconds < milliseconds && (done == null || !done())) { Application.DoEvents(); Thread.Sleep(15); }
    }
}

public sealed class BunkerLauncher : Form
{
    public const string Title = "Amnesia: The Bunker - Co-op online";
    const string SteamAppId = "1944430";
    static readonly string State = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BunkerOnlineCoop");

    readonly TextBox folder = new TextBox(), playerName = new TextBox(), address = new TextBox(), port = new TextBox();
    readonly RadioButton host = new RadioButton(), join = new RadioButton();

    public BunkerLauncher()
    {
        Text = Title; Icon = LauncherArt.WindowIcon();
        ClientSize = new Size(660, 330); FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen; Font = new Font("Segoe UI", 10);

        Controls.Add(new Label { Text = "Cartella di Amnesia: The Bunker", AutoSize = true, Location = new Point(20, 18) });
        folder.Location = new Point(20, 42); folder.Size = new Size(475, 28); Controls.Add(folder);
        var browse = new Button { Text = "Scegli cartella", Location = new Point(510, 40), Size = new Size(130, 32) };
        browse.Click += delegate
        {
            using (var dialog = new FolderBrowserDialog { Description = "Cartella che contiene AmnesiaTheBunker.exe" })
                if (dialog.ShowDialog(this) == DialogResult.OK) folder.Text = dialog.SelectedPath;
        };
        Controls.Add(browse);

        Controls.Add(new Label { Text = "Il tuo nome", AutoSize = true, Location = new Point(20, 88) });
        playerName.Location = new Point(130, 85); playerName.Size = new Size(200, 28); Controls.Add(playerName);

        host.Text = "Ospita la partita"; host.AutoSize = true; host.Location = new Point(20, 128); Controls.Add(host);
        join.Text = "Entra nella partita di:"; join.AutoSize = true; join.Location = new Point(20, 162); Controls.Add(join);
        address.Location = new Point(200, 160); address.Size = new Size(200, 28); Controls.Add(address);
        Controls.Add(new Label { Text = "Porta UDP", AutoSize = true, Location = new Point(420, 130) });
        port.Location = new Point(510, 127); port.Size = new Size(80, 28); Controls.Add(port);
        host.CheckedChanged += delegate { address.Enabled = join.Checked; };

        Controls.Add(new Label
        {
            Text = "L'host comunica il suo indirizzo (es. Radmin VPN 26.x.x.x) a chi entra. Se Windows\r\n"
                 + "chiede il permesso del firewall, consenti l'accesso. Poi nel gioco: carica o inizia una partita.",
            Location = new Point(20, 205), Size = new Size(620, 50)
        });
        var launch = new Button { Text = "Avvia co-op", Location = new Point(390, 275), Size = new Size(250, 34) };
        launch.Click += delegate { Start(); };
        Controls.Add(launch); AcceptButton = launch;

        var s = LoadSettings();
        folder.Text = Get(s, "folder", "");
        if (!Valid(folder.Text)) folder.Text = Discover();
        playerName.Text = Get(s, "name", Environment.UserName);
        address.Text = Get(s, "address", "");
        port.Text = Get(s, "port", Bridge.DefaultPort.ToString());
        if (Get(s, "role", "host") == "join") join.Checked = true; else host.Checked = true;
        address.Enabled = join.Checked;
    }

    // ------------------------------------------------------------------

    void Start()
    {
        int udpPort;
        try
        {
            if (!Valid(folder.Text)) throw new IOException("Questa cartella non contiene AmnesiaTheBunker.exe. Scegli la cartella del gioco installato da Steam.");
            if (playerName.Text.Trim() == "") throw new IOException("Scrivi un nome: lo vedrà l'altro giocatore.");
            if (!int.TryParse(port.Text, out udpPort) || udpPort < 1024 || udpPort > 65535) throw new IOException("La porta deve essere un numero tra 1024 e 65535.");
            if (join.Checked && address.Text.Trim() == "") throw new IOException("Scrivi l'indirizzo dell'host (es. 26.12.34.56).");
            if (Process.GetProcessesByName("steam").Length == 0) throw new IOException("Apri Steam con l'account che possiede Amnesia: The Bunker, poi riprova.");
            if (Process.GetProcessesByName("AmnesiaTheBunker").Length > 0) throw new IOException("Amnesia: The Bunker è già aperto: chiudilo e riprova.");
            SaveSettings();
            InstallMod(folder.Text);
        }
        catch (Exception e)
        {
            MessageBox.Show(this, e.Message, "Avvio non riuscito", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        var log = new List<string>();
        // The game uses OnlineCoop\<profile>\ when that folder exists, so a
        // bridge only ever talks to the game profile it launched.
        string profile = Profile();
        var options = new BridgeOptions
        {
            Role = host.Checked ? BridgeRole.Host : BridgeRole.Client,
            HostAddress = address.Text.Trim(), Port = udpPort, PlayerName = playerName.Text.Trim(),
            IpcDir = profile != null ? Path.Combine(Bridge.DefaultIpcDir, profile) : Bridge.DefaultIpcDir,
            Log = text => { lock (log) { log.Add(DateTime.Now.ToString("HH:mm:ss") + "  " + text); if (log.Count > 200) log.RemoveAt(0); } }
        };
        Bridge bridge;
        Process game;
        try
        {
            bridge = new Bridge(options);
            bridge.Start();
        }
        catch (SocketException e)
        {
            MessageBox.Show(this, "Impossibile usare la porta UDP " + udpPort + ": " + e.Message + "\r\nForse un'altra sessione è ancora aperta.",
                "Avvio non riuscito", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        catch (Exception e)
        {
            MessageBox.Show(this, e.Message, "Avvio non riuscito", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        try { game = LaunchGame(folder.Text, profile); }
        catch (Exception e)
        {
            bridge.Stop();
            MessageBox.Show(this, e.Message, "Avvio non riuscito", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        Hide();
        using (var splash = new LauncherSplash("Avvio di Amnesia: The Bunker..."))
        {
            splash.Show();
            LauncherSplash.Wait(30000, delegate { game.Refresh(); return game.HasExited || game.MainWindowHandle != IntPtr.Zero; });
        }
        using (var status = new SessionWindow(bridge, game, options, log)) status.ShowDialog();
        bridge.Stop();
        Close();
    }

    // ------------------------------------------------------------------
    // Add-on install: the package ships mod\OnlineCoop next to the launcher.

    static string ModSource()
    {
        string root = AppDomain.CurrentDomain.BaseDirectory;
        foreach (var candidate in new[] { Path.Combine(root, "mod", "OnlineCoop"), Path.GetFullPath(Path.Combine(root, "..", "mod", "OnlineCoop")) })
            if (File.Exists(Path.Combine(candidate, "entry.hpc"))) return candidate;
        throw new FileNotFoundException("Cartella mod\\OnlineCoop mancante: estrai tutto il pacchetto ZIP prima di avviare.");
    }

    static void InstallMod(string gameDir)
    {
        string source = ModSource(), target = Path.Combine(gameDir, "mods", "OnlineCoop");
        if (Directory.Exists(target) && SameTree(source, target)) { CopyPartnerAnimations(gameDir, target); WriteCoopEventHandler(gameDir, target); return; }
        if (Directory.Exists(target))
        {
            string backup = Path.Combine(State, "backups", "OnlineCoop_" + DateTime.Now.ToString("yyyyMMdd_HHmmss"));
            Directory.CreateDirectory(Path.GetDirectoryName(backup));
            Directory.Move(target, backup);
        }
        CopyTree(source, target);
        CopyPartnerAnimations(gameDir, target);
        WriteCoopEventHandler(gameDir, target);
    }

    // The add-on overrides the game's ModEventHandler by name, but the game's
    // script does not ship with the mod: the add-on's copy is rebuilt at every
    // launch from this installation plus the [COOP] lines listed in
    // patches\ModEventHandler.coop.txt (same rules as tools\InstallMod.ps1).
    static string CoopEventHandlerPath(string target)
    {
        return Path.Combine(target, "script", "modules", "ModEventHandler.hps");
    }

    static void WriteCoopEventHandler(string gameDir, string target)
    {
        string script = null;
        var ops = new List<string[]>();
        foreach (var raw in File.ReadAllLines(Path.Combine(target, "patches", "ModEventHandler.coop.txt")))
        {
            string l = raw.Trim();
            if (l == "" || l.StartsWith("#")) continue;
            var p = l.Split('|');
            if (p[0] == "source") script = p[1];
            else if (p[0] != "sha256") ops.Add(new[] { p[0], p[1], p[2].Replace("\\t", "\t") });
        }
        string text = File.ReadAllText(Path.Combine(gameDir, script.Replace('/', Path.DirectorySeparatorChar)));
        string nl = text.Contains("\r\n") ? "\r\n" : "\n";
        var lines = new List<string>(text.Split(new[] { nl }, StringSplitOptions.None));
        foreach (var op in ops)
        {
            int at = -1, hits = 0;
            for (int i = 0; i < lines.Count; i++) if (lines[i].Trim() == op[1]) { at = i; hits++; }
            if (hits != 1) throw new IOException("Lo script " + script + " del gioco non è quello previsto: aggiorna la mod.");
            if (op[0] == "after") lines.Insert(at + 1, op[2]);
            else if (op[0] == "before") lines.Insert(at, op[2]);
            else
            {
                int open = at + 1;
                while (open < lines.Count && lines[open].Trim() != "{") open++;
                if (open >= lines.Count) throw new IOException("Lo script " + script + " del gioco non è quello previsto: aggiorna la mod.");
                lines.Insert(open + 1, op[2]);
            }
        }
        lines.InsertRange(0, new[] {
            "// Generated by the OnlineCoop installer from this installation's " + script + ".",
            "// Lines marked [COOP] are the add-on's additions (GPL-3.0-or-later, see",
            "// patches/ModEventHandler.coop.txt); everything else is the game's own",
            "// script, which the add-on does not redistribute.",
            "" });
        string dest = CoopEventHandlerPath(target);
        Directory.CreateDirectory(Path.GetDirectoryName(dest));
        File.WriteAllText(dest, string.Join(nl, lines.ToArray()), new UTF8Encoding(false));
    }

    // The avatar's animations are private copies of the player's own, taken
    // from this installation (see tools/MakePartnerEntity.py): shared files
    // would break the real player's animations, and no game file ships with
    // the mod. Missing copies are (re)created at every launch.
    static void CopyPartnerAnimations(string gameDir, string target)
    {
        foreach (var line in File.ReadAllLines(Path.Combine(target, "entities", "coop", "anims.txt")))
        {
            string l = line.Trim();
            if (l == "" || l.StartsWith("#")) continue;
            var parts = l.Split('|');
            string dest = Path.Combine(target, parts[1].Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(dest)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(dest));
            File.Copy(Path.Combine(gameDir, parts[0].Replace('/', Path.DirectorySeparatorChar)), dest);
        }
    }

    static bool SameTree(string a, string b)
    {
        var fa = Directory.GetFiles(a, "*", SearchOption.AllDirectories);
        // The installed copy also holds the avatar animations copied from the
        // game (entities\coopnims) and the rebuilt ModEventHandler: not part
        // of the comparison.
        string anims = Path.Combine(b, "entities", "coop", "anims"), handler = CoopEventHandlerPath(b);
        var fb = Array.FindAll(Directory.GetFiles(b, "*", SearchOption.AllDirectories),
            f => !f.StartsWith(anims, StringComparison.OrdinalIgnoreCase) && !string.Equals(f, handler, StringComparison.OrdinalIgnoreCase));
        if (fa.Length != fb.Length) return false;
        foreach (var file in fa)
        {
            string other = Path.Combine(b, file.Substring(a.Length).TrimStart('\\', '/'));
            if (!File.Exists(other) || new FileInfo(other).Length != new FileInfo(file).Length) return false;
            if (!BytesEqual(File.ReadAllBytes(file), File.ReadAllBytes(other))) return false;
        }
        return true;
    }

    static bool BytesEqual(byte[] x, byte[] y)
    {
        if (x.Length != y.Length) return false;
        for (int i = 0; i < x.Length; i++) if (x[i] != y[i]) return false;
        return true;
    }

    static void CopyTree(string source, string target)
    {
        Directory.CreateDirectory(target);
        foreach (var dir in Directory.GetDirectories(source, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(target, dir.Substring(source.Length).TrimStart('\\', '/')));
        foreach (var file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
            File.Copy(file, Path.Combine(target, file.Substring(source.Length).TrimStart('\\', '/')), true);
    }

    // ------------------------------------------------------------------
    // Game start, mirroring the relaunch done by the game's Mods menu:
    //   AmnesiaTheBunker.exe -addonbatch "<file>" -user <profile>
    // with SteamAppId in the environment like a game started by Steam (an exe
    // started without it is restarted by Steam and loses its arguments).
    // Steam must be running; the game checks the licence as usual.

    static Process LaunchGame(string gameDir, string user)
    {
        // The game honours the batch only at its own location, the file its
        // Mods menu writes before relaunching; a batch elsewhere is ignored.
        string main = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "My Games", "Amnesia The Bunker", "Main");
        Directory.CreateDirectory(main);
        string batch = Path.Combine(main, "addons.batch");
        File.WriteAllText(batch, "<AddOnBatch>\r\n    <AddOn Locator=\"local:OnlineCoop\" />\r\n</AddOnBatch>\r\n", new UTF8Encoding(false));
        string args = " -addonbatch \"" + batch + "\"";
        if (user != null) args += " -user " + user;
        var start = new ProcessStartInfo(Path.Combine(gameDir, "AmnesiaTheBunker.exe"), args) { WorkingDirectory = gameDir, UseShellExecute = false };
        start.EnvironmentVariables["SteamAppId"] = SteamAppId;
        start.EnvironmentVariables["SteamGameId"] = SteamAppId;
        return Process.Start(start);
    }

    // The profile the game used last (it passes the same -user on relaunch);
    // none on a first run, and the game then picks or creates one itself.
    static string Profile()
    {
        string main = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "My Games", "Amnesia The Bunker", "Main");
        if (!Directory.Exists(main)) return null;
        FileInfo best = null;
        foreach (var f in new DirectoryInfo(main).GetFiles("*_user_settings.cfg"))
        {
            // CoopTest* profiles belong to the two-instance tests.
            if (f.Name.StartsWith("CoopTest", StringComparison.OrdinalIgnoreCase)) continue;
            if (best == null || f.LastWriteTimeUtc > best.LastWriteTimeUtc) best = f;
        }
        if (best == null) return null;
        string name = best.Name.Substring(0, best.Name.Length - "_user_settings.cfg".Length);
        return Regex.IsMatch(name, "^[A-Za-z0-9_]+$") ? name : null;
    }

    // ------------------------------------------------------------------

    public static bool Valid(string path)
    {
        return !string.IsNullOrEmpty(path) && File.Exists(Path.Combine(path, "AmnesiaTheBunker.exe")) && Directory.Exists(Path.Combine(path, "script"));
    }

    static string Discover()
    {
        var steam = Convert.ToString(Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", ""));
        var candidates = new List<string>();
        if (!string.IsNullOrEmpty(steam))
        {
            candidates.Add(Path.Combine(steam, "steamapps", "common", "Amnesia The Bunker"));
            var libraries = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
            if (File.Exists(libraries))
                foreach (Match m in Regex.Matches(File.ReadAllText(libraries), "\"path\"\\s*\"([^\"]+)\""))
                    candidates.Add(Path.Combine(m.Groups[1].Value.Replace(@"\\", @"\"), "steamapps", "common", "Amnesia The Bunker"));
        }
        foreach (var c in candidates) if (Valid(c)) return c;
        return "";
    }

    static Dictionary<string, string> LoadSettings()
    {
        var d = new Dictionary<string, string>();
        try
        {
            foreach (var line in File.ReadAllLines(Path.Combine(State, "settings.txt")))
            {
                int eq = line.IndexOf('=');
                if (eq > 0) d[line.Substring(0, eq)] = line.Substring(eq + 1);
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        return d;
    }

    static string Get(Dictionary<string, string> d, string key, string fallback) { string v; return d.TryGetValue(key, out v) && v != "" ? v : fallback; }

    void SaveSettings()
    {
        Directory.CreateDirectory(State);
        File.WriteAllLines(Path.Combine(State, "settings.txt"), new[]
        {
            "folder=" + folder.Text.Trim(), "name=" + playerName.Text.Trim(), "role=" + (join.Checked ? "join" : "host"),
            "address=" + address.Text.Trim(), "port=" + port.Text.Trim()
        });
    }

    [STAThread]
    public static void Main()
    {
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
        BunkerLauncher launcher;
        using (var splash = new LauncherSplash("Caricamento..."))
        {
            splash.Show(); splash.Update();
            var watch = Stopwatch.StartNew();
            launcher = new BunkerLauncher();
            LauncherSplash.Wait(1200 - (int)Math.Min(1200, watch.ElapsedMilliseconds), null);
        }
        Application.Run(launcher);
    }
}

// Small window kept open while playing: link state, round trip, the host's
// addresses to share, and the bridge log. Closing it, or the game exiting,
// ends the session.
sealed class SessionWindow : Form
{
    readonly Bridge bridge; readonly Process game; readonly List<string> log;
    readonly Label state = new Label(), detail = new Label();
    readonly TextBox logBox = new TextBox();
    readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer { Interval = 500 };
    int shownLines;

    public SessionWindow(Bridge bridge, Process game, BridgeOptions options, List<string> log)
    {
        this.bridge = bridge; this.game = game; this.log = log;
        Text = BunkerLauncher.Title; Icon = LauncherArt.WindowIcon(); Font = new Font("Segoe UI", 10);
        ClientSize = new Size(560, 330); StartPosition = FormStartPosition.CenterScreen; MinimizeBox = true; MaximizeBox = false;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        state.Font = new Font("Segoe UI", 13, FontStyle.Bold); state.Location = new Point(16, 12); state.Size = new Size(530, 30); Controls.Add(state);
        detail.Location = new Point(16, 46); detail.Size = new Size(530, 64); Controls.Add(detail);
        logBox.Multiline = true; logBox.ReadOnly = true; logBox.ScrollBars = ScrollBars.Vertical;
        logBox.Location = new Point(16, 116); logBox.Size = new Size(528, 160); logBox.Font = new Font("Consolas", 9); Controls.Add(logBox);
        var end = new Button { Text = "Termina sessione", Location = new Point(394, 286), Size = new Size(150, 32) };
        end.Click += delegate { Close(); };
        Controls.Add(end);
        string where = options.Role == BridgeRole.Host
            ? "Ospiti sulla porta UDP " + options.Port + ". Indirizzi da dare all'amico: " + LocalAddresses()
            : "Connessione a " + options.HostAddress;
        Controls.Add(new Label { Text = where, Location = new Point(16, 282), Size = new Size(370, 40), ForeColor = Color.DimGray });
        timer.Tick += delegate { Refresh2(); };
        timer.Start();
        Refresh2();
    }

    void Refresh2()
    {
        game.Refresh();
        if (game.HasExited) { Close(); return; }
        var s = bridge.GetStatus();
        switch (s.Link)
        {
            case LinkState.Connected: state.Text = "Connesso con " + s.PeerName; state.ForeColor = Color.ForestGreen; break;
            case LinkState.Rejected: state.Text = "Rifiutato: " + s.RejectReason; state.ForeColor = Color.Firebrick; break;
            case LinkState.Connecting: state.Text = "Connessione all'host..."; state.ForeColor = Color.DarkGoldenrod; break;
            default: state.Text = "In attesa del compagno..."; state.ForeColor = Color.DarkGoldenrod; break;
        }
        detail.Text = (s.Link == LinkState.Connected ? "Ping: " + s.RttMs + " ms   " + (s.PeerMap != "" ? "Il compagno è in: " + s.PeerMap : "Il compagno non è ancora in partita") + "\r\n" : "")
            + "Gioco: " + (s.GameRunning ? "mod attiva" : "nel menu o in caricamento (la mod si attiva in partita)");
        lock (log)
        {
            if (log.Count != shownLines) { logBox.Lines = log.ToArray(); shownLines = log.Count; logBox.SelectionStart = logBox.TextLength; logBox.ScrollToCaret(); }
        }
    }

    static string LocalAddresses()
    {
        var list = new List<string>();
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || nic.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
            foreach (var a in nic.GetIPProperties().UnicastAddresses)
                if (a.Address.AddressFamily == AddressFamily.InterNetwork) list.Add(a.Address.ToString());
        }
        return list.Count == 0 ? "-" : string.Join(", ", list.ToArray());
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        game.Refresh();
        if (!game.HasExited && e.CloseReason == CloseReason.UserClosing
            && MessageBox.Show(this, "Terminare la sessione co-op? Il gioco resta aperto ma senza collegamento.", BunkerLauncher.Title,
                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
        {
            e.Cancel = true; return;
        }
        timer.Stop();
        base.OnFormClosing(e);
    }
}
