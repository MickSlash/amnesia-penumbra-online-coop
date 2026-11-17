// SPDX-License-Identifier: GPL-3.0-or-later
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

// Artwork is embedded by tools/BuildLauncher.ps1 (/resource and /win32icon), so
// the launcher stays a single file. A missing resource must never block play.
static class LauncherArt {
    static Stream Open(string name) { return typeof(LauncherArt).Assembly.GetManifestResourceStream(name); }
    public static Icon WindowIcon() {
        var stream = Open("launcher.ico");
        return stream == null ? null : new Icon(stream);
    }
    public static Image Splash() {
        var stream = Open("launcher_splash.png");
        // Image.FromStream keeps reading the stream lazily: copy into a bitmap
        // that owns its pixels, then release the resource stream.
        if(stream == null) return null;
        using(stream) using(var image = Image.FromStream(stream)) return new Bitmap(image);
    }
}

// Launcher language: English by default, Italian on request. The choice is kept
// in language.txt beside game-folder.txt. The in-game online menu stays Italian,
// so the English help text names its buttons as they appear in the game.
static class Lang {
    public static bool Italian;
    public static string T(string english, string italian) { return Italian ? italian : english; }
    static string File(string state) { return Path.Combine(state,"language.txt"); }
    public static void Load(string state) {
        try { Italian = System.IO.File.Exists(File(state)) && System.IO.File.ReadAllText(File(state)).Trim() == "it"; }
        catch(IOException) { Italian = false; }
        catch(UnauthorizedAccessException) { Italian = false; }
    }
    public static void Save(string state) {
        try { Directory.CreateDirectory(state); System.IO.File.WriteAllText(File(state), Italian ? "it" : "en", new UTF8Encoding(false)); }
        catch(IOException) { }
        catch(UnauthorizedAccessException) { }
    }
}

// Borderless loading screen: shown while the launcher scans for the game and
// again while Lux starts, until the game opens its own window.
sealed class LauncherSplash : Form {
    readonly Image art = LauncherArt.Splash();
    string status;
    public LauncherSplash(string text) {
        status = text;
        FormBorderStyle = FormBorderStyle.None; StartPosition = FormStartPosition.CenterScreen;
        ShowInTaskbar = true; Text = Lang.T("Amnesia - Online Co-op","Amnesia - Campagna co-op"); Icon = LauncherArt.WindowIcon();
        BackColor = Color.Black; DoubleBuffered = true;
        // Logical size; WinForms scales it on high-DPI displays.
        ClientSize = new Size(420, 420);
        // The artwork is a rounded tile: clip the window to the same shape
        // (corner radius 19% of the side, matching the image frame).
        using(var shape = new GraphicsPath()) {
            int d = (int)(ClientSize.Width * 0.38f), w = ClientSize.Width, h = ClientSize.Height;
            shape.AddArc(0, 0, d, d, 180, 90); shape.AddArc(w - d, 0, d, d, 270, 90);
            shape.AddArc(w - d, h - d, d, d, 0, 90); shape.AddArc(0, h - d, d, d, 90, 90);
            shape.CloseFigure();
            Region = new Region(shape);
        }
    }
    public void SetStatus(string text) { status = text; Invalidate(); Update(); }
    protected override void OnPaint(PaintEventArgs e) {
        var g = e.Graphics;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        if(art != null) g.DrawImage(art, ClientRectangle);
        // Status band over the lower part of the art, readable on any frame.
        var band = new Rectangle(0, ClientSize.Height - 74, ClientSize.Width, 40);
        using(var shade = new SolidBrush(Color.FromArgb(170, 0, 0, 0))) g.FillRectangle(shade, band);
        using(var font = new Font("Segoe UI", 11, FontStyle.Bold))
        using(var format = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
            g.DrawString(status, font, Brushes.Gainsboro, band, format);
    }
    protected override void Dispose(bool disposing) { if(disposing && art != null) art.Dispose(); base.Dispose(disposing); }
    // Pump messages for a bounded time so the splash keeps painting.
    public static void Wait(int milliseconds, Func<bool> done) {
        var watch = Stopwatch.StartNew();
        while(watch.ElapsedMilliseconds < milliseconds && (done == null || !done())) { Application.DoEvents(); Thread.Sleep(15); }
    }
}

public sealed class CoopLauncher : Form {
    readonly TextBox folder = new TextBox();
    readonly CheckBox fullscreen = new CheckBox();
    readonly CheckBox steamPresence = new CheckBox();
    readonly Label intro = new Label(), help = new Label();
    readonly Button browse = new Button(), launch = new Button();
    readonly ComboBox language = new ComboBox();
    readonly string state = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AmnesiaOnlineCoop");
    readonly string saveFolder = "AmnesiaOnlineCoop_Host";
    public CoopLauncher(string testProfile = null) {
        if(testProfile != null) {
            if(!Regex.IsMatch(testProfile,"^OnlineCoopMenuQA_[a-zA-Z0-9_]+$")) throw new ArgumentException("Invalid QA profile");
            state = Path.Combine(state,testProfile); saveFolder = testProfile;
        }
        Lang.Load(state);
        ClientSize = new Size(660,225); Icon = LauncherArt.WindowIcon();
        FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI",10);
        intro.AutoSize = true; intro.Location = new Point(20,20); Controls.Add(intro);
        language.DropDownStyle = ComboBoxStyle.DropDownList; language.Location = new Point(510,16); language.Size = new Size(130,28);
        language.Items.AddRange(new object[] { "English", "Italiano" }); language.SelectedIndex = Lang.Italian ? 1 : 0;
        language.SelectedIndexChanged += delegate { Lang.Italian = language.SelectedIndex == 1; Lang.Save(state); ApplyLanguage(); };
        Controls.Add(language);
        folder.Location = new Point(20,57); folder.Size = new Size(475,28); Controls.Add(folder);
        browse.Location = new Point(510,55); browse.Size = new Size(130,32);
        browse.Click += delegate { using(var dialog = new FolderBrowserDialog()) { dialog.Description = Lang.T("Folder containing config, maps and entities","Cartella contenente config, maps ed entities"); if(dialog.ShowDialog(this) == DialogResult.OK) folder.Text = dialog.SelectedPath; } }; Controls.Add(browse);
        fullscreen.AutoSize = true; fullscreen.Location = new Point(20,99); Controls.Add(fullscreen);
        steamPresence.AutoSize = true; steamPresence.Location = new Point(200,99); Controls.Add(steamPresence);
        folder.TextChanged += delegate { steamPresence.Checked = SteamInstallation(folder.Text); };
        help.Location = new Point(20,130); help.Size = new Size(620,50); Controls.Add(help);
        launch.Location = new Point(390,185); launch.Size = new Size(250,32);
        launch.Click += delegate {
            Process game;
            try { game = Launch(folder.Text, fullscreen.Checked, steamPresence.Checked); }
            catch(Exception error) { MessageBox.Show(this, error.Message, Lang.T("Launch failed","Avvio non riuscito"), MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
            Hide();
            // Lux needs a few seconds before its window exists; keep the art on
            // screen meanwhile. Give up after 30 s: the game keeps loading anyway.
            using(game) using(var splash = new LauncherSplash(Lang.T("Starting Amnesia...","Avvio di Amnesia..."))) {
                splash.Show();
                LauncherSplash.Wait(30000, delegate { game.Refresh(); return game.HasExited || game.MainWindowHandle != IntPtr.Zero; });
            }
            Close();
        }; Controls.Add(launch); AcceptButton = launch;
        ApplyLanguage();
        var saved = Path.Combine(state,"game-folder.txt");
        if(File.Exists(saved)) folder.Text = File.ReadAllText(saved).Trim();
        if(!Valid(folder.Text)) folder.Text = Discover();
    }
    void ApplyLanguage() {
        Text = Lang.T("Amnesia - Online Co-op","Amnesia - Campagna co-op");
        intro.Text = Lang.T("Select your Amnesia: The Dark Descent installation.","Seleziona la tua installazione di Amnesia: The Dark Descent.");
        browse.Text = Lang.T("Browse...","Scegli cartella");
        fullscreen.Text = Lang.T("Fullscreen","Schermo intero");
        steamPresence.Text = Lang.T("Show Amnesia on Steam","Mostra Amnesia su Steam");
        help.Text = Lang.T("In game: Co-op online > Ospita (host) or Connetti (join) > both press Pronto (ready).\r\nThe host then picks Nuova campagna (new) or Continua salvataggio (continue).",
            "Nel gioco: Co-op online > Ospita / Connetti > entrambi Pronto.\r\nL'host sceglie Nuova campagna o Continua salvataggio.");
        launch.Text = Lang.T("Launch Amnesia co-op","Avvia Amnesia co-op");
    }
    public static bool Valid(string path) {
        return !String.IsNullOrEmpty(path) && File.Exists(Path.Combine(path,"config","main_init.cfg")) && File.Exists(Path.Combine(path,"resources.cfg")) && Directory.Exists(Path.Combine(path,"maps")) && Directory.Exists(Path.Combine(path,"entities"));
    }
    public static bool SteamInstallation(string path) {
        if(!Valid(path)) return false;
        try { return File.Exists(Path.Combine(path,"steam_api64.dll")) && File.ReadAllText(Path.Combine(path,"steam_appid.txt")).Trim() == "57300"; }
        catch(IOException) { return false; }
        catch(UnauthorizedAccessException) { return false; }
    }
    static string Discover() {
        var steam = Convert.ToString(Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", ""));
        var candidates = new System.Collections.Generic.List<string>();
        if(!String.IsNullOrEmpty(steam)) {
            candidates.Add(Path.Combine(steam,"steamapps","common","Amnesia The Dark Descent"));
            var libraries = Path.Combine(steam,"steamapps","libraryfolders.vdf");
            if(File.Exists(libraries)) foreach(Match match in Regex.Matches(File.ReadAllText(libraries), "\"path\"\\s*\"([^\"]+)\"")) candidates.Add(Path.Combine(match.Groups[1].Value.Replace(@"\\",@"\"),"steamapps","common","Amnesia The Dark Descent"));
        }
        foreach(var candidate in candidates) if(Valid(candidate)) return candidate;
        // Do not block startup on disconnected network drives or optical media.
        foreach(var drive in DriveInfo.GetDrives()) if(drive.DriveType == DriveType.Fixed) {
            var candidate = Path.Combine(drive.RootDirectory.FullName,"SteamLibrary","steamapps","common","Amnesia The Dark Descent");
            if(Valid(candidate)) return candidate;
        }
        return "";
    }
    public Process Launch(string path, bool full, bool steam) {
        if(!Valid(path)) throw new IOException(Lang.T("This folder does not contain the Amnesia game data. Select the folder of the original game.","Questa cartella non contiene i dati di Amnesia. Scegli la cartella del gioco originale."));
        path = Path.GetFullPath(path);
        if(steam) {
            if(!SteamInstallation(path)) throw new IOException(Lang.T("This folder does not contain the original x64 Steam runtime of Amnesia. Verify the game files in Steam or untick Show Amnesia on Steam.","Questa cartella non contiene il runtime Steam x64 originale di Amnesia. Verifica i file del gioco su Steam oppure disattiva Mostra Amnesia su Steam."));
            var clients = Process.GetProcessesByName("steam");
            bool running = clients.Length > 0;
            foreach(var client in clients) client.Dispose();
            if(!running) throw new IOException(Lang.T("Open Steam with the account that owns Amnesia, then try again. Or untick Show Amnesia on Steam.","Apri Steam con l'account che possiede Amnesia, poi riprova. In alternativa disattiva Mostra Amnesia su Steam."));
        }
        string root = AppDomain.CurrentDomain.BaseDirectory;
        string executable = Path.Combine(root,"bin","Lux.exe");
        if(!File.Exists(executable)) executable = Path.GetFullPath(Path.Combine(root,"..","amnesia","src","game","x64","Release","Lux.exe"));
        if(!File.Exists(executable)) throw new FileNotFoundException(Lang.T("Lux.exe is missing: extract the whole ZIP package before launching.","Lux.exe mancante: estrai tutto il pacchetto ZIP prima di avviare."));
        Directory.CreateDirectory(state); File.WriteAllText(Path.Combine(state,"game-folder.txt"),path,Encoding.UTF8);
        string settings = Path.Combine(state,"settings.cfg"), init = Path.Combine(state,"init.cfg");
        File.WriteAllText(settings,"<Main ShowMenu=\"true\" ShowPreMenu=\"false\" SaveConfig=\"true\" DefaultProfileName=\"online\" UpdateLogActive=\"false\" />\n<Graphics TextureQuality=\"0\" Shadows=\"1\" SSAOActive=\"true\" />\n<Engine LimitFPS=\"true\" />\n<Screen Width=\"1280\" Height=\"720\" Display=\"0\" FullScreen=\"" + (full ? "true" : "false") + "\" Vsync=\"true\" />\n<Physics PhysicsAccuracy=\"2\" UpdatesPerSec=\"60\" />\n<Online AutoProfile=\"true\" />\n",new UTF8Encoding(false));
        string source = File.ReadAllText(Path.Combine(path,"config","main_init.cfg"));
        source = Regex.Replace(source,"MainSaveFolder\\s*=\\s*\"[^\"]*\"", "MainSaveFolder = \"" + saveFolder + "\"");
        if(saveFolder.StartsWith("OnlineCoopMenuQA_")) source = Regex.Replace(source,"GameName\\s*=\\s*\"[^\"]*\"", "GameName = \"Amnesia Menu QA [" + saveFolder + "]\"");
        source = Regex.Replace(source,"(DefaultMainSettings(?:SDL2)?)\\s*=\\s*\"[^\"]*\"",delegate(Match match) { return match.Groups[1].Value + " = \"" + System.Security.SecurityElement.Escape(settings) + "\""; });
        File.WriteAllText(init,source,new UTF8Encoding(false));
        var start = new ProcessStartInfo(executable,"\"" + init + "\"") { WorkingDirectory = path, UseShellExecute = false };
        var keys = new System.Collections.Generic.List<string>();
        foreach(string key in start.EnvironmentVariables.Keys) if(key.StartsWith("AMNESIA_NET_",StringComparison.OrdinalIgnoreCase)) keys.Add(key);
        foreach(string key in keys) start.EnvironmentVariables.Remove(key);
        start.EnvironmentVariables.Remove("AMNESIA_STEAM_PRESENCE");
        start.EnvironmentVariables.Remove("SteamAppId");
        start.EnvironmentVariables.Remove("SteamGameId");
        if(steam) {
            start.EnvironmentVariables["AMNESIA_STEAM_PRESENCE"] = "1";
            start.EnvironmentVariables["SteamAppId"] = "57300";
            start.EnvironmentVariables["SteamGameId"] = "57300";
        }
        return Process.Start(start);
    }
    [STAThread] public static void Main(string[] args) {
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
        string testProfile = args.Length == 2 && args[0] == "--test-profile" ? args[1] : null;
        Lang.Load(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AmnesiaOnlineCoop"));
        CoopLauncher launcher;
        using(var splash = new LauncherSplash(Lang.T("Loading...","Caricamento..."))) {
            splash.Show(); splash.Update();
            // The constructor may scan Steam libraries and fixed drives.
            var watch = Stopwatch.StartNew();
            launcher = new CoopLauncher(testProfile);
            // Keep the art visible briefly even on fast PCs; QA runs skip the pause.
            if(testProfile == null) LauncherSplash.Wait(1200 - (int)Math.Min(1200, watch.ElapsedMilliseconds), null);
        }
        Application.Run(launcher);
    }
}
