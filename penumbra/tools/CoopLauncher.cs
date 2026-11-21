// SPDX-License-Identifier: GPL-3.0-or-later
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;

public sealed class CoopLauncher : Form {
    readonly TextBox folder = new TextBox(), address = new TextBox();
    readonly NumericUpDown port = new NumericUpDown();
    readonly CheckBox fullscreen = new CheckBox();
    readonly CheckBox useSteam = new CheckBox();
    readonly CheckBox resume = new CheckBox();
    readonly NumericUpDown players = new NumericUpDown();
    readonly TextBox nickname = new TextBox();
    readonly CheckBox lobby = new CheckBox();
    readonly string state = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"PenumbraOnlineCoop");
    public CoopLauncher() {
        Text="Penumbra: Overture — Co-op sperimentale"; ClientSize=new Size(690,409); Font=new Font("Segoe UI",10);
        FormBorderStyle=FormBorderStyle.FixedDialog; MaximizeBox=false; StartPosition=FormStartPosition.CenterScreen;
        Controls.Add(new Label { Text="Cartella di Penumbra: Overture", Location=new Point(20,18), AutoSize=true });
        folder.SetBounds(20,47,485,28); Controls.Add(folder);
        var browse=new Button { Text="Scegli cartella", Location=new Point(520,44), Size=new Size(150,32) };
        browse.Click+=delegate { using(var dialog=new FolderBrowserDialog()) if(dialog.ShowDialog(this)==DialogResult.OK) folder.Text=dialog.SelectedPath; }; Controls.Add(browse);
        Controls.Add(new Label { Text="IP dell'host (LAN o VPN)", Location=new Point(20,93), AutoSize=true });
        address.SetBounds(20,122,290,28); address.Text="127.0.0.1"; Controls.Add(address);
        Controls.Add(new Label { Text="Porta UDP", Location=new Point(330,93), AutoSize=true });
        port.SetBounds(330,122,110,28); port.Minimum=1; port.Maximum=65535; port.Value=27120; Controls.Add(port);
        fullscreen.Text="Schermo intero"; fullscreen.SetBounds(465,122,205,28); Controls.Add(fullscreen);
        useSteam.Text="Usa Steam: mostra Penumbra: Overture in esecuzione"; useSteam.SetBounds(20,166,650,28); Controls.Add(useSteam);
        folder.TextChanged+=delegate { useSteam.Checked=SteamInstallation(folder.Text); };
        resume.Text="Host: riprendi l\x27ultimo checkpoint cooperativo"; resume.SetBounds(20,202,420,28); Controls.Add(resume);
        // Giocatori in totale, host compreso: lo sceglie solo l'host.
        Controls.Add(new Label { Text="Giocatori (host)", Location=new Point(465,205), AutoSize=true });
        players.SetBounds(600,202,70,28); players.Minimum=2; players.Maximum=4; players.Value=2; Controls.Add(players);
        // Nickname mostrato nella lobby; lobby prepartita (pronto, avvio dell'host).
        Controls.Add(new Label { Text="Nickname", Location=new Point(20,242), AutoSize=true });
        nickname.SetBounds(110,239,250,28); nickname.MaxLength=24; Controls.Add(nickname);
        lobby.Text="Lobby prepartita (Pronto / Avvia)"; lobby.SetBounds(390,239,280,28); Controls.Add(lobby);
        Controls.Add(new Label { Text="Da 2 a 4 giocatori, nemici e salvataggio autorevoli sull\x27host. Nella lobby: R = pronto,\r\nINVIO (host) = avvia quando tutti sono pronti. Avatar provvisorio, campagna da validare.", Location=new Point(20,279), Size=new Size(650,52) });
        var host=new Button { Text="Ospita partita", Location=new Point(20,353), Size=new Size(205,36) };
        var join=new Button { Text="Connettiti all'host", Location=new Point(240,353), Size=new Size(220,36) };
        var logs=new Button { Text="Apri log della mod", Location=new Point(475,353), Size=new Size(195,36) };
        host.Click+=delegate { Start("host"); }; join.Click+=delegate { Start("client"); };
        logs.Click+=delegate { Directory.CreateDirectory(state); Process.Start(new ProcessStartInfo(state) { UseShellExecute=true }); };
        Controls.Add(host); Controls.Add(join); Controls.Add(logs);
        var saved=Path.Combine(state,"game-folder.txt"); if(File.Exists(saved)) folder.Text=File.ReadAllText(saved).Trim();
        if(!Valid(folder.Text)) folder.Text=Discover();
        var preference=Path.Combine(state,"use-steam.txt"); if(File.Exists(preference)) useSteam.Checked=File.ReadAllText(preference).Trim()=="1";
        // Schermo intero come nel gioco originale, e la scelta viene ricordata.
        var screen=Path.Combine(state,"fullscreen.txt"); fullscreen.Checked=!File.Exists(screen) || File.ReadAllText(screen).Trim()=="1";
        var name=Path.Combine(state,"nickname.txt"); nickname.Text=File.Exists(name)?File.ReadAllText(name).Trim():Environment.UserName;
        var lobbyChoice=Path.Combine(state,"lobby.txt"); lobby.Checked=!File.Exists(lobbyChoice) || File.ReadAllText(lobbyChoice).Trim()=="1";
        var count=Path.Combine(state,"players.txt"); int savedPlayers;
        if(File.Exists(count) && Int32.TryParse(File.ReadAllText(count).Trim(),out savedPlayers) && savedPlayers>=2 && savedPlayers<=4) players.Value=savedPlayers;
    }
    public static string DataDirectory(string path) {
        if(String.IsNullOrWhiteSpace(path)) return "";
        path=Path.GetFullPath(path.Trim());
        if(File.Exists(Path.Combine(path,"config","default_settings.cfg"))) return path;
        return Path.Combine(path,"redist");
    }
    public static bool Valid(string path) {
        try { path=DataDirectory(path); return File.Exists(Path.Combine(path,"config","game.cfg")) && File.Exists(Path.Combine(path,"resources.cfg")) && Directory.Exists(Path.Combine(path,"maps")); }
        catch(ArgumentException) { return false; } catch(NotSupportedException) { return false; }
    }
    static string Discover() {
        var steam=Convert.ToString(Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam","SteamPath",""));
        var candidates=new System.Collections.Generic.List<string>();
        if(!String.IsNullOrEmpty(steam)) {
            candidates.Add(Path.Combine(steam,"steamapps","common","Penumbra Overture"));
            var libraries=Path.Combine(steam,"steamapps","libraryfolders.vdf");
            if(File.Exists(libraries)) foreach(Match match in Regex.Matches(File.ReadAllText(libraries),"\"path\"\\s*\"([^\"]+)\"")) candidates.Add(Path.Combine(match.Groups[1].Value.Replace(@"\\",@"\"),"steamapps","common","Penumbra Overture"));
        }
        foreach(var candidate in candidates) if(Valid(candidate)) return candidate;
        return "";
    }
    public static bool SteamInstallation(string path) {
        try {
            var data=new DirectoryInfo(DataDirectory(path)); var game=data.Name.Equals("redist",StringComparison.OrdinalIgnoreCase)?data.Parent:data;
            if(game==null || game.Parent==null || !game.Parent.Name.Equals("common",StringComparison.OrdinalIgnoreCase)) return false;
            var manifest=Path.Combine(game.Parent.Parent.FullName,"appmanifest_22180.acf");
            return File.Exists(manifest) && Regex.IsMatch(File.ReadAllText(manifest),"\"installdir\"\\s*\""+Regex.Escape(game.Name)+"\"",RegexOptions.IgnoreCase);
        } catch { return false; }
    }
    static bool ValveRuntime32(string path) {
        try {
            if(!File.Exists(path)) return false;
            using(var stream=File.OpenRead(path)) using(var reader=new BinaryReader(stream)) {
                stream.Position=0x3c; var pe=reader.ReadInt32(); if(pe<0 || pe>stream.Length-6) return false;
                stream.Position=pe; if(reader.ReadUInt32()!=0x4550 || reader.ReadUInt16()!=0x14c) return false;
            }
            var cert=X509Certificate.CreateFromSignedFile(path);
            return cert.Subject.IndexOf("O=Valve",StringComparison.OrdinalIgnoreCase)>=0;
        } catch { return false; }
    }
    public static string SteamRuntime(string gamePath) {
        var candidates=new System.Collections.Generic.List<string>();
        candidates.Add(Path.Combine(DataDirectory(gamePath),"steam_api.dll"));
        var steam=Convert.ToString(Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam","SteamPath",""));
        var libraries=new System.Collections.Generic.List<string>(); if(!String.IsNullOrEmpty(steam)) libraries.Add(steam);
        if(!String.IsNullOrEmpty(steam)) {
            var vdf=Path.Combine(steam,"steamapps","libraryfolders.vdf");
            if(File.Exists(vdf)) foreach(Match m in Regex.Matches(File.ReadAllText(vdf),"\"path\"\\s*\"([^\"]+)\"")) libraries.Add(m.Groups[1].Value.Replace(@"\\",@"\"));
        }
        foreach(var library in libraries) {
            var common=Path.Combine(library,"steamapps","common"); if(!Directory.Exists(common)) continue;
            candidates.Add(Path.Combine(common,"Amnesia The Dark Descent","steam_api.dll"));
            foreach(var directory in Directory.GetDirectories(common)) {
                candidates.Add(Path.Combine(directory,"steam_api.dll")); candidates.Add(Path.Combine(directory,"bin","steam_api.dll")); candidates.Add(Path.Combine(directory,"Binaries","Win32","steam_api.dll"));
            }
        }
        foreach(var candidate in candidates) if(ValveRuntime32(candidate)) return candidate;
        return "";
    }
    void Start(string role) {
        try {
            if(!Valid(folder.Text)) throw new IOException("Scegli la cartella del gioco originale, oppure la sua sottocartella redist.");
            if(role=="client" && (String.IsNullOrWhiteSpace(address.Text) || address.Text.IndexOfAny(new[]{'\r','\n',' ','\t'})>=0)) throw new IOException("Inserisci un indirizzo IP o un nome host valido.");
            var root=AppDomain.CurrentDomain.BaseDirectory;
            var exe=Path.Combine(root,"bin","PenumbraCoop.exe"); if(!File.Exists(exe)) throw new FileNotFoundException("PenumbraCoop.exe mancante: estrai tutto lo ZIP prima di avviare.");
            Directory.CreateDirectory(state); File.WriteAllText(Path.Combine(state,"game-folder.txt"),folder.Text,Encoding.UTF8);
            var launch=new ProcessStartInfo(exe) { WorkingDirectory=DataDirectory(folder.Text), UseShellExecute=false };
            launch.EnvironmentVariables.Remove("SteamAppId"); launch.EnvironmentVariables.Remove("SteamGameId");
            var keys=new System.Collections.Generic.List<string>();
            foreach(string key in launch.EnvironmentVariables.Keys) if(key.StartsWith("PENUMBRA_COOP_",StringComparison.OrdinalIgnoreCase)) keys.Add(key);
            foreach(string key in keys) launch.EnvironmentVariables.Remove(key);
            launch.EnvironmentVariables["PENUMBRA_COOP_ROLE"]=role;
            if(role=="host") {
                launch.EnvironmentVariables["PENUMBRA_COOP_PLAYERS"]=players.Value.ToString();
                File.WriteAllText(Path.Combine(state,"players.txt"),players.Value.ToString(),Encoding.UTF8);
            }
            if(role=="host" && resume.Checked) {
                var checkpoint=Path.Combine(state,"host","PenumbraOnlineCoop","Overture","save","coop_checkpoint.sav");
                if(!File.Exists(checkpoint)) throw new IOException("Non esiste ancora un checkpoint co-op su questo PC. Avvia una nuova partita e salva dal menu.");
                launch.EnvironmentVariables["PENUMBRA_COOP_RESUME"]="1";
            }
            launch.EnvironmentVariables["PENUMBRA_COOP_ADDRESS"]=address.Text.Trim();
            launch.EnvironmentVariables["PENUMBRA_COOP_PORT"]=port.Value.ToString();
            launch.EnvironmentVariables["PENUMBRA_COOP_FULLSCREEN"]=fullscreen.Checked?"1":"0";
            launch.EnvironmentVariables["PENUMBRA_COOP_PROFILE_ROOT"]=Path.Combine(state,role);
            var clean=new StringBuilder(); foreach(var c in nickname.Text.Trim()) if(c>=' ' && c<='~' && clean.Length<24) clean.Append(c);
            if(clean.Length==0) throw new IOException("Inserisci un nickname (lettere e numeri senza accenti, al massimo 24).");
            launch.EnvironmentVariables["PENUMBRA_COOP_NAME"]=clean.ToString();
            launch.EnvironmentVariables["PENUMBRA_COOP_LOBBY"]=lobby.Checked?"1":"0";
            File.WriteAllText(Path.Combine(state,"nickname.txt"),clean.ToString(),Encoding.UTF8);
            File.WriteAllText(Path.Combine(state,"lobby.txt"),lobby.Checked?"1":"0");
            File.WriteAllText(Path.Combine(state,"fullscreen.txt"),fullscreen.Checked?"1":"0");
            ImportOriginalSettings(role);
            if(useSteam.Checked) {
                if(!SteamInstallation(folder.Text)) throw new IOException("Usa Steam richiede l'installazione originale di Penumbra: Overture nella libreria Steam. Per la versione DVD disattiva l'opzione.");
                var clients=Process.GetProcessesByName("steam"); bool running=clients.Length>0; foreach(var client in clients) client.Dispose();
                if(!running) throw new IOException("Apri Steam con l'account che possiede Penumbra: Overture, poi riprova oppure disattiva Usa Steam.");
                var runtime=SteamRuntime(folder.Text); if(runtime.Length==0) throw new IOException("Nelle librerie Steam non è stato trovato un runtime Valve a 32 bit. Per ora disattiva Usa Steam.");
                launch.EnvironmentVariables["PENUMBRA_COOP_STEAM"]="1"; launch.EnvironmentVariables["PENUMBRA_COOP_STEAM_API"]=runtime;
                launch.EnvironmentVariables["SteamAppId"]="22180"; launch.EnvironmentVariables["SteamGameId"]="22180";
            }
            File.WriteAllText(Path.Combine(state,"use-steam.txt"),useSteam.Checked?"1":"0");
            Process.Start(launch); Close();
        } catch(Exception error) { MessageBox.Show(this,error.Message,"Avvio non riuscito",MessageBoxButtons.OK,MessageBoxIcon.Error); }
    }
    // Una volta per profilo: risoluzione, grafica, audio e tasti del gioco
    // originale (Documenti\Penumbra Overture\Episode1\settings.cfg). La
    // configurazione precedente della mod resta come copia di sicurezza.
    void ImportOriginalSettings(string role) {
        try {
            var marker=Path.Combine(state,role+"-settings-imported.txt"); if(File.Exists(marker)) return;
            var original=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),"Penumbra Overture","Episode1","settings.cfg");
            var profile=Path.Combine(state,role,"PenumbraOnlineCoop","Overture"); var target=Path.Combine(profile,"settings.cfg");
            if(File.Exists(original)) {
                Directory.CreateDirectory(profile);
                if(File.Exists(target)) File.Copy(target,target+".prima-dell-importazione",true);
                File.Copy(original,target,true);
            }
            File.WriteAllText(marker,File.Exists(original)?original:"originale non trovato");
        } catch(IOException) { } catch(UnauthorizedAccessException) { }
    }
    [STAThread] public static void Main(string[] args) {
        if(args.Length==2 && args[0]=="--validate") { Environment.Exit(Valid(args[1])?0:1); return; }
        if(args.Length==2 && args[0]=="--validate-steam") { Environment.Exit(SteamInstallation(args[1]) && SteamRuntime(args[1]).Length>0?0:1); return; }
        if(args.Length==2 && args[0]=="--render") {
            Application.EnableVisualStyles();
            using(var form=new CoopLauncher()) using(var bitmap=new Bitmap(form.Width,form.Height)) {
                form.StartPosition=FormStartPosition.Manual; form.Location=new Point(-32000,-32000); form.ShowInTaskbar=false;
                form.Show(); Application.DoEvents(); form.DrawToBitmap(bitmap,new Rectangle(0,0,bitmap.Width,bitmap.Height)); bitmap.Save(args[1]);
            }
            return;
        }
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false); Application.Run(new CoopLauncher());
    }
}
