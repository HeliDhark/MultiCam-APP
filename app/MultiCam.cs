using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Win32;

namespace MultiCam {
    public class Mode {
        public int Width, Height, Format;
        public long Interval;
        public double Fps { get { return Interval > 0 ? 10000000.0 / Interval : 0; } }
        public string Key { get { return Width + "/" + Height + "/" + Interval + "/" + Format; } }
        public override string ToString() {
            string format;
            switch (Format) { case 301: format="YUY2"; break; case 400: format="MJPEG"; break; case 201: format="NV12"; break; case 101: format="RGB32"; break; case 102: format="RGB24"; break; default: format="formato " + Format; break; }
            return Width + " × " + Height + " · " + Fps.ToString("0.##",CultureInfo.InvariantCulture) + " FPS · " + format;
        }
        public static List<Mode> DisplayChoices(IEnumerable<Mode> available, Mode selected) {
            // Group by the complete visible label, including resolution and pixel format.
            // Keep the selected exact interval so applying a name/setting never switches it.
            // The full driver catalog remains available for capture validation.
            return available.GroupBy(m=>m.ToString(),StringComparer.Ordinal)
                .Select(group=>group.FirstOrDefault(m=>selected!=null && m.Key==selected.Key) ?? group.First()).ToList();
        }
    }
    public class Device { public string Name,Path,Identity,IdentityKind,Location; }
    public class Camera {
        public string Identity, Id, Alias, LastName, LastPath, IdentityKind, Location;
        public bool Enabled = true;
        public Mode Mode;
        public List<Mode> Modes = new List<Mode>();
        public PipSettings Pip = new PipSettings();
    }
    public class Config { public bool AutoStart; public List<Camera> Cameras = new List<Camera>(); public WallpaperSettings Wallpaper = new WallpaperSettings(); }
    public class Stats { public long Frames, Fps, Dropped; }
    public static class Storage {
        public static readonly string Root=System.IO.Path.GetDirectoryName(typeof(Storage).Assembly.Location);
        public static readonly string Data=System.IO.Path.Combine(Root,"data");
        public static readonly JavaScriptSerializer Json=new JavaScriptSerializer();
        public static Config Load() {
            Directory.CreateDirectory(Data);
            var path=System.IO.Path.Combine(Data,"settings.json");
            if (!File.Exists(path)) { var fresh=new Config(); ViewerSettings.Normalize(fresh); return fresh; }
            var config=Json.Deserialize<Config>(File.ReadAllText(path,Encoding.UTF8));
            if (config==null || config.Cameras==null) throw new InvalidDataException("Configuração inválida. Restaure data/settings.json.bak.");
            foreach (var c in config.Cameras) { Guid.Parse(c.Id); if(c.Modes==null)c.Modes=new List<Mode>(); }
            ViewerSettings.Normalize(config);
            return config;
        }
        public static void Save(Config config) {
            Directory.CreateDirectory(Data);
            var path=System.IO.Path.Combine(Data,"settings.json");
            File.WriteAllText(path+".tmp",Json.Serialize(config),new UTF8Encoding(false));
            if(File.Exists(path)) File.Replace(path+".tmp",path,path+".bak"); else File.Move(path+".tmp",path);
        }
        public static void Log(string text) {
            try { Directory.CreateDirectory(Data); var p=System.IO.Path.Combine(Data,"multicam.log");
                if(File.Exists(p) && new FileInfo(p).Length>2000000) File.Move(p,p+"."+DateTime.Now.ToString("yyyyMMddHHmmss"));
                File.AppendAllText(p,DateTime.Now.ToString("s")+" "+text+Environment.NewLine,Encoding.UTF8);
            } catch { }
        }
    }
    public static class Native {
        public static string Worker { get { return System.IO.Path.Combine(Storage.Root,"x64","MultiCam.Worker.exe"); } }
        public static string Quote(string value) {
            if(value==null)value="";
            var b=new StringBuilder("\""); int slashes=0;
            foreach(char c in value) { if(c=='\\') { slashes++; continue; }
                if(c=='\"') { b.Append('\\',slashes*2+1); b.Append(c); slashes=0; continue; }
                b.Append('\\',slashes); slashes=0; b.Append(c);
            }
            b.Append('\\',slashes*2); return b.Append('"').ToString();
        }
        public static ProcessStartInfo Info(string arguments) { return new ProcessStartInfo(Worker,arguments) {
            UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,
            StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8,WorkingDirectory=Storage.Root
        }; }
        public static async Task<string> Run(string arguments) {
            using(var process=new Process { StartInfo=Info(arguments) }) {
                process.Start();
                var output=process.StandardOutput.ReadToEndAsync(); var error=process.StandardError.ReadToEndAsync();
                bool finished=await Task.Run(()=>process.WaitForExit(15000));
                if(!finished) { try {process.Kill();} catch {} throw new IOException("A consulta à placa demorou mais de 15 segundos."); }
                var result=await output; var details=await error;
                if(process.ExitCode!=0) throw new IOException(details.Trim());
                return result;
            }
        }
    }
    public static class Registration {
        const string Category="{860BB310-5D01-11D0-BD3B-00A0C911CE86}";
        static string Cls(Camera c) { return new Guid(c.Id).ToString("B").ToUpperInvariant(); }
        public static void Publish(Camera c) {
            string cls=Cls(c);
            using(var settings=Registry.CurrentUser.CreateSubKey("Software\\MultiCam\\Cameras\\"+cls)) {
                settings.SetValue("Name",c.Alias+" (Multi)"); settings.SetValue("Width",c.Mode.Width);
                settings.SetValue("Height",c.Mode.Height); settings.SetValue("Interval",(int)c.Mode.Interval);
            }
            try {
                foreach(var view in new[]{RegistryView.Registry64,RegistryView.Registry32}) {
                    string arch=view==RegistryView.Registry64?"x64":"x86";
                    string dll=System.IO.Path.Combine(Storage.Root,arch,"MultiCam.Camera.dll");
                    if(!File.Exists(dll)) throw new FileNotFoundException("Componente da câmera ausente",dll);
                    using(var root=RegistryKey.OpenBaseKey(RegistryHive.CurrentUser,view)) {
                        using(var server=root.CreateSubKey("Software\\Classes\\CLSID\\"+cls+"\\InprocServer32")) {
                            server.SetValue("",dll); server.SetValue("ThreadingModel","Both");
                        }
                        using(var instance=root.CreateSubKey("Software\\Classes\\CLSID\\"+Category+"\\Instance\\"+cls)) {
                            instance.SetValue("CLSID",cls); instance.SetValue("FriendlyName",c.Alias+" (Multi)");
                        }
                    }
                }
            } catch { Remove(c,false); throw; }
        }
        public static void Remove(Camera c,bool full) {
            string cls=Cls(c);
            foreach(var view in new[]{RegistryView.Registry64,RegistryView.Registry32}) {
                using(var root=RegistryKey.OpenBaseKey(RegistryHive.CurrentUser,view)) {
                    root.DeleteSubKeyTree("Software\\Classes\\CLSID\\"+Category+"\\Instance\\"+cls,false);
                    if(full) root.DeleteSubKeyTree("Software\\Classes\\CLSID\\"+cls,false);
                }
            }
            if(full) Registry.CurrentUser.DeleteSubKeyTree("Software\\MultiCam\\Cameras\\"+cls,false);
        }
        public static void Startup(bool enabled) {
            using(var key=Registry.CurrentUser.CreateSubKey("Software\\Microsoft\\Windows\\CurrentVersion\\Run")) {
                if(enabled)key.SetValue("MultiCam",Native.Quote(Application.ExecutablePath)+" --tray"); else key.DeleteValue("MultiCam",false);
            }
        }
    }
    class Session : IDisposable {
        public readonly Camera Camera;
        public Process Process;
        public volatile bool Ready;
        public string Error="";
        public long Fps,Frames;
        public bool Published;
        public DateTime Started=DateTime.UtcNow;
        public Session(Camera c) {
            Camera=c; var m=c.Mode;
            var args="--capture "+Native.Quote(c.LastPath)+" "+Native.Quote(new Guid(c.Id).ToString("B").ToUpperInvariant())+" "+m.Width+" "+m.Height+" "+m.Interval+" "+m.Format+" "+System.Diagnostics.Process.GetCurrentProcess().Id+" "+Native.Quote(c.LastName);
            Process=new Process { StartInfo=Native.Info(args) };
            Process.OutputDataReceived+=(s,e)=> { if(String.IsNullOrWhiteSpace(e.Data)) return;
                try { var stats=new JavaScriptSerializer().Deserialize<Stats>(e.Data); Interlocked.Exchange(ref Fps,stats.Fps); Interlocked.Exchange(ref Frames,stats.Frames); Ready=stats.Frames>0; } catch { }
            };
            Process.ErrorDataReceived+=(s,e)=> { if(!String.IsNullOrWhiteSpace(e.Data)) { Error=e.Data; Storage.Log(c.Alias+": "+e.Data); } };
            Process.Start(); Process.BeginOutputReadLine(); Process.BeginErrorReadLine();
        }
        public bool Exited { get { try { return Process.HasExited; } catch { return true; } } }
        public void Dispose() {
            try { using(var stop=EventWaitHandle.OpenExisting("Local\\MultiCam-"+new Guid(Camera.Id).ToString("B").ToUpperInvariant()+"-stop"))stop.Set(); } catch {}
            try { if(!Process.WaitForExit(2000))Process.Kill(); } catch {} Process.Dispose();
        }
    }
    public class MainForm : Form {
        Config config;
        Dictionary<string,Session> sessions=new Dictionary<string,Session>();
        Dictionary<string,string> errors=new Dictionary<string,string>();
        Dictionary<string,DateTime> retry=new Dictionary<string,DateTime>();
        Dictionary<string,int> failures=new Dictionary<string,int>();
        Dictionary<string,DateTime> missing=new Dictionary<string,DateTime>();
        HashSet<string> connected=new HashSet<string>();
        // Cameras whose virtual camera is registered. It outlives a restart of the capture worker,
        // so PiP and wallpaper stay open (last frame, then black) while the worker comes back.
        HashSet<string> registered=new HashSet<string>();
        // Capture on demand: a card runs while its camera is in use and for a minute after.
        CaptureScheduler scheduler=new CaptureScheduler();
        // Demand areas of registered cameras, created by the panel so sandboxed apps can write them.
        Dictionary<string,IDisposable> demandAreas=new Dictionary<string,IDisposable>();
        DemandWaker waker;
        bool wakePending;
        NotifyIcon tray;
        System.Windows.Forms.Timer timer=new System.Windows.Forms.Timer { Interval=2000 };
        ListView list=new ListView { Dock=DockStyle.Fill,View=View.Details,FullRowSelect=true,HideSelection=false,MultiSelect=false };
        TextBox alias=new TextBox { Width=280,MaxLength=80 };
        ComboBox modes=new ComboBox { Width=390,DropDownStyle=ComboBoxStyle.DropDownList };
        CheckBox enabled=new CheckBox { Text="Compartilhar esta placa automaticamente",AutoSize=true };
        CheckBox startup=new CheckBox { Text="Iniciar com o Windows",AutoSize=true };
        Label detail=new Label { Dock=DockStyle.Fill,AutoEllipsis=true,Padding=new Padding(12),ForeColor=Color.DimGray };
        Label status=new Label { Dock=DockStyle.Bottom,Height=34,Padding=new Padding(12,8,0,0) };
        // PiP of the selected camera (applied immediately) and the wallpaper section.
        ViewerManager viewers=new ViewerManager(System.IO.Path.Combine(Storage.Root,"x64","MultiCam.Viewer.exe"),Process.GetCurrentProcess().Id);
        CheckBox pipEnabled=new CheckBox { Text="PiP ativo",AutoSize=true };
        CheckBox pipTop=new CheckBox { Text="Sempre à frente",AutoSize=true };
        CheckBox pipLock=new CheckBox { Text="Travar",AutoSize=true };
        ComboBox pipSize=new ComboBox { Width=130,DropDownStyle=ComboBoxStyle.DropDownList };
        Button pipRestore=new Button { Text="Restaurar posição",AutoSize=true };
        CheckBox wallEnabled=new CheckBox { Text="Ativar",AutoSize=true };
        ComboBox wallCamera=new ComboBox { Width=220,DropDownStyle=ComboBoxStyle.DropDownList };
        ComboBox wallMonitor=new ComboBox { Width=260,DropDownStyle=ComboBoxStyle.DropDownList };
        RadioButton wall30=new RadioButton { Text="30 FPS",AutoSize=true }, wall60=new RadioButton { Text="60 FPS",AutoSize=true };
        RadioButton wallFill=new RadioButton { Text="Preencher",AutoSize=true }, wallContain=new RadioButton { Text="Ajustar",AutoSize=true };
        Label wallStatus=new Label { AutoSize=true,ForeColor=Color.DimGray,Padding=new Padding(8,5,0,0) };
        bool busy,exiting,loading,closePending;
        Camera Selected { get { return list.SelectedItems.Count>0?(Camera)list.SelectedItems[0].Tag:null; } }
        public MainForm(Config state,bool hidden) {
            config=state; Text="MultiCam · Compartilhar câmeras"; MinimumSize=new Size(1040,760); Size=new Size(1180,840);
            Icon=System.Drawing.Icon.ExtractAssociatedIcon(typeof(MainForm).Assembly.Location);
            Font=new Font("Segoe UI",10); StartPosition=FormStartPosition.CenterScreen;
            var title=new Label { Dock=DockStyle.Top,Height=82,Text="MultiCam\nUma placa. Vários programas. Até o FPS real da captura.",Font=new Font("Segoe UI",14),Padding=new Padding(16) };
            foreach(var col in new[]{new ColumnHeader { Text="Câmera Multi",Width=205 },new ColumnHeader {Text="Nome original",Width=160},new ColumnHeader{Text="Estado",Width=180},new ColumnHeader{Text="Captura configurada",Width=310},new ColumnHeader{Text="FPS recebidos",Width=110},new ColumnHeader{Text="Exibição",Width=170}})list.Columns.Add(col);
            var editor=new FlowLayoutPanel { Dock=DockStyle.Bottom,Height=150,Padding=new Padding(12),WrapContents=true };
            editor.Controls.Add(new Label {Text="Nome:",AutoSize=true,Padding=new Padding(0,5,0,0)}); editor.Controls.Add(alias);
            editor.Controls.Add(new Label {Text="Modo:",AutoSize=true,Padding=new Padding(10,5,0,0)}); editor.Controls.Add(modes);
            editor.SetFlowBreak(modes,true); editor.Controls.Add(enabled);
            var apply=new Button {Text="Aplicar",AutoSize=true}; apply.Click+=async(s,e)=>await Apply(); editor.Controls.Add(apply);
            var refresh=new Button {Text="Reler modos",AutoSize=true}; refresh.Click+=async(s,e)=>await ReloadModes(); editor.Controls.Add(refresh);
            var folder=new Button {Text="Abrir diagnóstico",AutoSize=true}; folder.Click+=(s,e)=>Process.Start("explorer.exe",Native.Quote(Storage.Data)); editor.Controls.Add(folder);
            editor.Controls.Add(startup);
            editor.SetFlowBreak(startup,true);
            editor.Controls.Add(new Label {Text="PiP:",AutoSize=true,Padding=new Padding(0,5,0,0)});
            editor.Controls.Add(pipEnabled); editor.Controls.Add(pipTop); editor.Controls.Add(pipLock);
            editor.Controls.Add(new Label {Text="Tamanho:",AutoSize=true,Padding=new Padding(10,5,0,0)}); editor.Controls.Add(pipSize);
            editor.Controls.Add(pipRestore);
            var wall=new GroupBox { Text="Papel de parede ao vivo",Dock=DockStyle.Bottom,Height=78,Padding=new Padding(10,4,10,4) };
            var wallRow=new FlowLayoutPanel { Dock=DockStyle.Fill,WrapContents=false };
            wallRow.Controls.Add(wallEnabled);
            wallRow.Controls.Add(new Label {Text="Câmera:",AutoSize=true,Padding=new Padding(10,5,0,0)}); wallRow.Controls.Add(wallCamera);
            wallRow.Controls.Add(new Label {Text="Monitor:",AutoSize=true,Padding=new Padding(10,5,0,0)}); wallRow.Controls.Add(wallMonitor);
            var fpsGroup=new FlowLayoutPanel { AutoSize=true,WrapContents=false,Margin=new Padding(10,0,0,0) }; fpsGroup.Controls.Add(wall30); fpsGroup.Controls.Add(wall60);
            var fitGroup=new FlowLayoutPanel { AutoSize=true,WrapContents=false,Margin=new Padding(10,0,0,0) }; fitGroup.Controls.Add(wallFill); fitGroup.Controls.Add(wallContain);
            wallRow.Controls.Add(fpsGroup); wallRow.Controls.Add(fitGroup); wallRow.Controls.Add(wallStatus);
            wall.Controls.Add(wallRow);
            var detailsPanel=new Panel {Dock=DockStyle.Bottom,Height=100}; detailsPanel.Controls.Add(detail);
            Controls.Add(list); Controls.Add(detailsPanel); Controls.Add(editor); Controls.Add(wall); Controls.Add(status); Controls.Add(title);
            pipEnabled.CheckedChanged+=(s,e)=>EditPip(p=>p.Enabled=pipEnabled.Checked);
            pipTop.CheckedChanged+=(s,e)=>EditPip(p=>p.TopMost=pipTop.Checked);
            pipLock.CheckedChanged+=(s,e)=>EditPip(p=>p.Locked=pipLock.Checked);
            pipSize.SelectedIndexChanged+=(s,e)=> { var size=pipSize.SelectedItem as SizeChoice; if(size!=null)EditPip(p=> {p.Width=size.Size.Width;p.Height=size.Size.Height;}); };
            pipRestore.Click+=(s,e)=>EditPip(p=>p.HasPosition=false);
            wallEnabled.CheckedChanged+=(s,e)=>EditWallpaper();
            wallCamera.SelectedIndexChanged+=(s,e)=>EditWallpaper();
            wallMonitor.SelectedIndexChanged+=(s,e)=>EditWallpaper();
            foreach(var r in new[]{wall30,wall60,wallFill,wallContain}) r.CheckedChanged+=(s,e)=> { if(((RadioButton)s).Checked)EditWallpaper(); };
            ShowWallpaper();
            list.SelectedIndexChanged+=(s,e)=>ShowSelected();
            startup.Checked=config.AutoStart; startup.CheckedChanged+=(s,e)=> {
                if(loading)return; try { Registration.Startup(startup.Checked); config.AutoStart=startup.Checked; Storage.Save(config); }
                catch(Exception ex) { MessageBox.Show(this,ex.Message,"Inicialização"); }
            };
            var menu=new ContextMenuStrip(); menu.Items.Add("Configurações",null,(s,e)=>ShowWindow());
            menu.Items.Add("Sair e liberar as placas",null,(s,e)=>ExitApp());
            tray=new NotifyIcon {Icon=Icon,Text="MultiCam",ContextMenuStrip=menu,Visible=true}; tray.DoubleClick+=(s,e)=>ShowWindow();
            timer.Tick+=async(s,e)=>await RefreshCameras();
            Shown+=async(s,e)=> { if(hidden)Hide(); waker=new DemandWaker(()=> { try { BeginInvoke((Action)Wake); } catch {} }); await RefreshCameras(); timer.Start(); };
            FormClosing+=(s,e)=> { if(!exiting) {e.Cancel=true;Hide();} };
            Application.ApplicationExit+=(s,e)=>Cleanup();
            Microsoft.Win32.SystemEvents.SessionEnding+=(s,e)=>Cleanup();
        }
        void ShowWindow() { Show(); WindowState=FormWindowState.Normal; Activate(); }
        public void Diagnose() {
            Opacity=0;ShowInTaskbar=false;
            var diagnosticTimer=new System.Windows.Forms.Timer {Interval=12000};
            diagnosticTimer.Tick+=(sender,args)=> {
                if(busy)return;diagnosticTimer.Stop();
                try {
                    string folder=System.IO.Path.GetFullPath(System.IO.Path.Combine(Storage.Root,"..","artifacts","ui"));Directory.CreateDirectory(folder);
                    using(var bitmap=new Bitmap(Width,Height)) {DrawToBitmap(bitmap,new Rectangle(Point.Empty,Size));bitmap.Save(System.IO.Path.Combine(folder,"panel.png"));}
                    var report=config.Cameras.Select(c=> {
                        Session s;sessions.TryGetValue(c.Id,out s);string error;errors.TryGetValue(c.Id,out error);
                        return new {Name=c.Alias,Original=c.LastName,Present=connected.Contains(c.Identity),Published=s!=null&&s.Published,Fps=s==null?0:Interlocked.Read(ref s.Fps),Mode=c.Mode==null?null:c.Mode.ToString(),Error=error};
                    }).ToArray();
                    File.WriteAllText(System.IO.Path.Combine(folder,"report.json"),Storage.Json.Serialize(report),Encoding.UTF8);
                }catch(Exception ex){Storage.Log(ex.ToString());}
                ExitApp();diagnosticTimer.Dispose();
            };
            diagnosticTimer.Start();
        }
        protected override void WndProc(ref Message m) { base.WndProc(ref m); if(m.Msg==0x219 && !busy) timer.Interval=500; }
        // An app started using a camera: refresh now, or right after the refresh in progress.
        void Wake() { if(busy) wakePending=true; else timer.Interval=50; }
        void ShowSelected() {
            var c=Selected; loading=true; modes.Items.Clear();
            if(c!=null) {
                alias.Text=c.Alias; enabled.Checked=c.Enabled;
                foreach(var mode in Mode.DisplayChoices(c.Modes,c.Mode))modes.Items.Add(mode);
                if(c.Mode!=null)for(int i=0;i<modes.Items.Count;i++)if(((Mode)modes.Items[i]).Key==c.Mode.Key)modes.SelectedIndex=i;
                string error; errors.TryGetValue(c.Id,out error);
                detail.Text="Identificação: "+c.IdentityKind+" · "+c.Id.Substring(0,8)+"\n"+
                    (c.IdentityKind=="serial do dispositivo"?"Nome associado ao serial informado pelo dispositivo.":"Sem serial USB único confirmado. Mantenha a placa na mesma porta; trocar de porta pode exigir renomear.")+"\n"+
                    (String.IsNullOrEmpty(error)?"Selecione “"+c.Alias+" (Multi)” nos aplicativos. Ao trocar o modo, reabra a câmera nos aplicativos.":error)+
                    (viewers.Status(c).StartsWith("Falhou")?"\nPiP: "+viewers.Status(c):"");
            } else { alias.Text=""; enabled.Checked=false; detail.Text="Conecte uma placa USB. As câmeras físicas e virtuais permanecem como entradas separadas nos aplicativos."; }
            loading=false;
            ShowPip();
        }
        class SizeChoice { public Size Size; public override string ToString() { return Size.Width+" × "+Size.Height; } }
        class CameraChoice { public Camera Camera; public override string ToString() { return Camera.Alias; } }
        class MonitorChoice { public string Device,Text; public override string ToString() { return Text; } }
        void ShowPip() {
            var c=Selected; loading=true;
            foreach(Control control in new Control[]{pipEnabled,pipTop,pipLock,pipSize,pipRestore}) control.Enabled=c!=null;
            pipSize.Items.Clear();
            if(c!=null) {
                pipEnabled.Checked=c.Pip.Enabled; pipTop.Checked=c.Pip.TopMost; pipLock.Checked=c.Pip.Locked;
                foreach(var size in ViewerSettings.Presets(c.Mode)) {
                    pipSize.Items.Add(new SizeChoice {Size=size});
                    if(size.Width==c.Pip.Width&&size.Height==c.Pip.Height)pipSize.SelectedIndex=pipSize.Items.Count-1;
                }
            } else { pipEnabled.Checked=pipTop.Checked=pipLock.Checked=false; }
            loading=false;
        }
        void EditPip(Action<PipSettings> change) {
            var c=Selected; if(loading||c==null)return;
            change(c.Pip);
            try { Storage.Save(config); } catch(Exception ex) { Storage.Log(ex.ToString()); }
            ReconcileViewers();
        }
        static string MonitorText(Screen screen,int index) {
            int w=screen.Bounds.Width,h=screen.Bounds.Height;
            var mode=new DevMode(); mode.dmSize=(short)System.Runtime.InteropServices.Marshal.SizeOf(typeof(DevMode));
            if(EnumDisplaySettings(screen.DeviceName,-1,ref mode)) { w=mode.dmPelsWidth; h=mode.dmPelsHeight; }
            return "Monitor "+(index+1)+" · "+w+" × "+h+(screen.Primary?" (principal)":"");
        }
        [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential,CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
        struct DevMode {
            [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr,SizeConst=32)] public string dmDeviceName;
            public short dmSpecVersion,dmDriverVersion,dmSize,dmDriverExtra; public int dmFields,dmPositionX,dmPositionY,dmDisplayOrientation,dmDisplayFixedOutput;
            public short dmColor,dmDuplex,dmYResolution,dmTTOption,dmCollate;
            [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr,SizeConst=32)] public string dmFormName;
            public short dmLogPixels; public int dmBitsPerPel,dmPelsWidth,dmPelsHeight,dmDisplayFlags,dmDisplayFrequency,dmICMMethod,dmICMIntent,dmMediaType,dmDitherType,dmReserved1,dmReserved2,dmPanningWidth,dmPanningHeight;
        }
        [System.Runtime.InteropServices.DllImport("user32.dll",CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
        static extern bool EnumDisplaySettings(string device,int mode,ref DevMode info);
        // Rebuilds the wallpaper controls from the settings (camera names and monitors may have changed).
        void ShowWallpaper() {
            var w=config.Wallpaper; loading=true;
            wallEnabled.Checked=w.Enabled;
            wallCamera.Items.Clear();
            foreach(var c in config.Cameras) { wallCamera.Items.Add(new CameraChoice {Camera=c}); if(c.Id==w.CameraId)wallCamera.SelectedIndex=wallCamera.Items.Count-1; }
            wallMonitor.Items.Clear();
            var screens=Screen.AllScreens;
            for(int i=0;i<screens.Length;i++) {
                wallMonitor.Items.Add(new MonitorChoice {Device=screens[i].DeviceName,Text=MonitorText(screens[i],i)});
                if(String.Equals(screens[i].DeviceName,w.Monitor,StringComparison.OrdinalIgnoreCase))wallMonitor.SelectedIndex=i;
            }
            wall30.Checked=w.Fps!=60; wall60.Checked=w.Fps==60;
            wallFill.Checked=w.Fit!="contain"; wallContain.Checked=w.Fit=="contain";
            loading=false;
            ShowWallpaperStatus();
        }
        void ShowWallpaperStatus() {
            var w=config.Wallpaper; string state=viewers.WallpaperStatus();
            if(!w.Enabled) state="";
            else if(state=="") {
                int fps=viewers.WallpaperFps();
                state=w.CameraId==null?"Escolha a câmera.":w.Monitor==null?"Escolha o monitor.":fps>0?"Desenhando "+fps+" FPS":"Aguardando a câmera…";
            }
            wallStatus.Text=state; wallStatus.ForeColor=state.StartsWith("Falhou")?Color.Firebrick:Color.DimGray;
        }
        void EditWallpaper() {
            if(loading)return;
            var w=config.Wallpaper;
            w.Enabled=wallEnabled.Checked;
            var camera=wallCamera.SelectedItem as CameraChoice; w.CameraId=camera==null?null:camera.Camera.Id;
            if(w.Enabled && camera==null && Selected!=null) { w.CameraId=Selected.Id; }
            var monitor=wallMonitor.SelectedItem as MonitorChoice; w.Monitor=monitor==null?null:monitor.Device;
            if(w.Enabled && monitor==null) w.Monitor=Screen.PrimaryScreen.DeviceName;
            w.Fps=wall60.Checked?60:30; w.Fit=wallContain.Checked?"contain":"fill";
            try { Storage.Save(config); } catch(Exception ex) { Storage.Log(ex.ToString()); }
            ShowWallpaper();
            ReconcileViewers();
        }
        void ReconcileViewers() {
            try {
                if(viewers.Reconcile(config,c=>registered.Contains(c.Id))) {
                    Storage.Save(config);
                    if(!pipSize.DroppedDown)ShowPip();
                }
            } catch(Exception ex) { Storage.Log(ex.ToString()); }
            ShowWallpaperStatus();
        }
        static string Display(Camera c,Config config) {
            bool pip=c.Pip!=null&&c.Pip.Enabled, wall=config.Wallpaper!=null&&config.Wallpaper.Enabled&&config.Wallpaper.CameraId==c.Id;
            return pip&&wall?"PiP · Papel de parede":pip?"PiP":wall?"Papel de parede":"";
        }
        static Mode DefaultMode(List<Mode> values) {
            return values.OrderBy(m=>m.Width==1920&&m.Height==1080?0:1).ThenBy(m=>Math.Abs(m.Fps-60))
                .ThenBy(m=>m.Format==301?0:m.Format==400?1:2).ThenByDescending(m=>m.Width*m.Height).FirstOrDefault();
        }
        async Task ReadModes(Camera c) {
            c.Modes=new JavaScriptSerializer().Deserialize<List<Mode>>(await Native.Run("--modes "+Native.Quote(c.LastPath)))
                .Where(m=>m.Width>0 && m.Height>0 && m.Interval>0).OrderByDescending(m=>m.Width).ThenByDescending(m=>m.Height).ThenByDescending(m=>m.Fps).ToList();
            if(c.Mode==null) c.Mode=DefaultMode(c.Modes);
            if(c.Mode!=null && !c.Modes.Any(m=>m.Key==c.Mode.Key))throw new IOException("O modo salvo não está disponível nesta conexão. Escolha outro modo; não reduziremos o FPS automaticamente.");
        }
        void Stop(Camera c) {
            Registration.Remove(c,false);
            registered.Remove(c.Id);
            scheduler.Forget(c.Id);
            IDisposable area; if(demandAreas.TryGetValue(c.Id,out area)) { area.Dispose(); demandAreas.Remove(c.Id); }
            EndSession(c);
        }
        // Ends only the capture worker. The virtual camera stays registered so open
        // applications keep their device and recover when the worker restarts.
        void EndSession(Camera c) {
            Session session; if(sessions.TryGetValue(c.Id,out session)) {session.Dispose();sessions.Remove(c.Id);}
        }
        async Task RefreshCameras() {
            if(busy||exiting)return; busy=true; timer.Interval=2000;
            try {
                var devices=new JavaScriptSerializer().Deserialize<List<Device>>(await Native.Run("--list"));
                // Cloned serials must never merge two live physical devices.
                foreach(var group in devices.GroupBy(d=>d.Identity,StringComparer.OrdinalIgnoreCase).Where(g=>g.Count()>1))
                    foreach(var d in group) { d.Identity=d.Path.ToLowerInvariant();d.IdentityKind="porta USB (serial repetido)"; }
                connected=new HashSet<string>(devices.Select(d=>d.Identity),StringComparer.OrdinalIgnoreCase);
                // A brief enumeration gap (USB hiccup, driver reset) must not drop the virtual camera.
                foreach(var c in config.Cameras) {
                    if(connected.Contains(c.Identity)) {missing.Remove(c.Id);continue;}
                    DateTime since; if(!missing.TryGetValue(c.Id,out since)) {missing[c.Id]=DateTime.UtcNow;continue;}
                    if(DateTime.UtcNow-since>=TimeSpan.FromSeconds(10))Stop(c);
                }
                bool changed=false;
                foreach(var d in devices) {
                    var c=config.Cameras.FirstOrDefault(v=>String.Equals(v.Identity,d.Identity,StringComparison.OrdinalIgnoreCase));
                    if(c==null) { c=new Camera {Identity=d.Identity,Id=Guid.NewGuid().ToString(),Alias="Câmera "+(config.Cameras.Count+1),LastName=d.Name,LastPath=d.Path,IdentityKind=d.IdentityKind,Location=d.Location}; config.Cameras.Add(c); changed=true; }
                    bool moved=c.LastPath!=d.Path;
                    if(moved) {Stop(c);c.Modes.Clear();errors.Remove(c.Id);changed=true;}
                    c.LastName=d.Name;c.LastPath=d.Path;c.IdentityKind=d.IdentityKind;c.Location=d.Location;
                    if(!c.Enabled) {Stop(c);continue;}
                    Session session;
                    var action=scheduler.Decide(c.Id,registered.Contains(c.Id),Demand.Active(c.Id),sessions.ContainsKey(c.Id),DateTime.UtcNow);
                    if(sessions.TryGetValue(c.Id,out session)) {
                        if(session.Exited) {
                            errors[c.Id]=String.IsNullOrEmpty(session.Error)?"Captura encerrada. Confira o modo e feche o acesso direto à placa.":session.Error;
                            int count; failures.TryGetValue(c.Id,out count); failures[c.Id]=++count;
                            if(CaptureScheduler.Unregister(count,registered.Contains(c.Id))) Stop(c); else EndSession(c);
                            retry[c.Id]=DateTime.UtcNow.AddSeconds(count>=3?10:2);
                        }
                        else if(session.Ready&&!session.Published) { Registration.Publish(c); registered.Add(c.Id); scheduler.Touch(c.Id,DateTime.UtcNow);
                            if(!demandAreas.ContainsKey(c.Id)) try { demandAreas[c.Id]=Demand.Prepare(c.Id); } catch(Exception ex) { Storage.Log("Área de demanda: "+ex.Message); } session.Published=true; errors.Remove(c.Id); failures.Remove(c.Id); }
                        // No app, PiP or wallpaper for a minute: stop the card, keep the camera registered.
                        else if(action==CaptureAction.Stop) EndSession(c);
                        continue;
                    }
                    if(action!=CaptureAction.Start)continue;
                    DateTime next; if(retry.TryGetValue(c.Id,out next)&&next>DateTime.UtcNow)continue;
                    try {
                        if(c.Modes.Count==0) {await ReadModes(c);changed=true;}
                        if(c.Mode==null || !c.Modes.Any(m=>m.Key==c.Mode.Key))throw new IOException("Selecione um modo de captura disponível.");
                        // Persist identity before registering anything, so interrupted runs can clean it up.
                        Storage.Save(config);
                        sessions[c.Id]=new Session(c);
                    } catch(Exception ex) {errors[c.Id]=ex.Message;retry[c.Id]=DateTime.UtcNow.AddSeconds(10);Storage.Log(ex.ToString());}
                }
                if(changed)Storage.Save(config);
                if(changed && wallCamera.Items.Count!=config.Cameras.Count && !wallCamera.DroppedDown)ShowWallpaper();
                ReconcileViewers();
                RenderList();status.Text=devices.Count+" placa(s) USB · "+registered.Count+" compartilhada(s) · Fechar a janela mantém o programa na bandeja.";
            } catch(Exception ex) {status.Text=ex.Message;Storage.Log(ex.ToString());}
            finally {busy=false;if(wakePending) {wakePending=false;timer.Interval=50;}if(closePending)ExitApp();}
        }
        // Status column of the list. An error wins over "Em espera": a registered camera whose
        // capture fails must not look idle while apps show black.
        public static string StateText(bool connected,bool enabled,bool running,bool published,bool registered,bool error) {
            if(!connected)return "Desconectada";
            if(!enabled)return "Desativada";
            if(running)return published?"Compartilhando":"Iniciando…";
            if(error)return "Precisa de atenção";
            return registered?"Em espera":"Detectada";
        }
        void RenderList() {
            var selected=Selected;list.BeginUpdate();
            foreach(var c in config.Cameras) {
                Session s;sessions.TryGetValue(c.Id,out s);
                string state=StateText(connected.Contains(c.Identity),c.Enabled,s!=null,s!=null&&s.Published,registered.Contains(c.Id),errors.ContainsKey(c.Id));
                string view=Display(c,config); string viewState=viewers.Status(c); if(viewState.StartsWith("Falhou"))view="PiP falhou";
                var values=new[]{c.Alias+" (Multi)",c.LastName,state,c.Mode==null?"Selecione um modo":c.Mode.ToString(),s==null?"—":Interlocked.Read(ref s.Fps).ToString(),view};
                var item=list.Items.Cast<ListViewItem>().FirstOrDefault(v=>((Camera)v.Tag).Id==c.Id);
                if(item==null) {item=new ListViewItem(values){Tag=c};list.Items.Add(item);}
                else for(int i=0;i<values.Length;i++)item.SubItems[i].Text=values[i];
                item.ForeColor=state=="Precisa de atenção"?Color.Firebrick:Color.Black;
            }
            list.EndUpdate(); if(selected==null && list.Items.Count>0)list.Items[0].Selected=true;
        }
        async Task Apply() {
            if(busy||Selected==null)return; var c=Selected; var mode=modes.SelectedItem as Mode;
            string name=alias.Text.Trim();
            if(name.Length==0 || name.Any(Char.IsControl)) {MessageBox.Show(this,"Informe um nome válido.");return;}
            if(config.Cameras.Any(v=>v.Id!=c.Id&&String.Equals(v.Alias,name,StringComparison.OrdinalIgnoreCase))) {MessageBox.Show(this,"Escolha um nome diferente para distinguir as placas.");return;}
            busy=true;
            try {Stop(c);c.Alias=name;c.Enabled=enabled.Checked;if(mode!=null)c.Mode=mode;Storage.Save(config);errors.Remove(c.Id);retry.Remove(c.Id);}
            catch(Exception ex){MessageBox.Show(this,ex.Message,"MultiCam");}
            finally {busy=false;}
            ShowWallpaper();
            await RefreshCameras();
        }
        async Task ReloadModes() {
            if(busy||Selected==null)return;var c=Selected;if(!connected.Contains(c.Identity))return;busy=true;
            try {Stop(c);await ReadModes(c);Storage.Save(config);errors.Remove(c.Id);retry.Remove(c.Id);}
            catch(Exception ex){errors[c.Id]=ex.Message;Storage.Log(ex.ToString());}
            finally{busy=false;ShowSelected();}
            await RefreshCameras();
        }
        void Cleanup() {timer.Stop();if(waker!=null) {waker.Dispose();waker=null;}try{viewers.StopAll();}catch(Exception ex){Storage.Log(ex.Message);}foreach(var c in config.Cameras)try{Stop(c);}catch(Exception ex){Storage.Log(ex.Message);}if(tray!=null)tray.Visible=false;}
        void ExitApp() {if(busy){closePending=true;return;}exiting=true;Cleanup();Close();Application.Exit();}
    }
    static class Program {
        [STAThread] static void Main(string[] args) {
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            bool created;
            using(var singleton=new Mutex(true,"Local\\MultiCam-App",out created)) {
                if(!created) {MessageBox.Show("O MultiCam já está rodando. Abra Configurações pelo ícone na bandeja.");return;}
                try {
                    var config=Storage.Load();
                    foreach(var c in config.Cameras)Registration.Remove(c,false);
                    if(args.Contains("--uninstall")) { foreach(var c in config.Cameras)Registration.Remove(c,true);Registration.Startup(false);config.AutoStart=false;Storage.Save(config);MessageBox.Show("Câmeras virtuais e inicialização removidas. Seus nomes foram preservados em data/settings.json.");return; }
                    var form=new MainForm(config,args.Contains("--tray"));
                    if(args.Contains("--diagnose"))form.Diagnose();
                    Application.Run(form);
                } catch(Exception ex) {Storage.Log(ex.ToString());MessageBox.Show(ex.Message,"MultiCam — não foi possível iniciar",MessageBoxButtons.OK,MessageBoxIcon.Error);}
            }
        }
    }
}
