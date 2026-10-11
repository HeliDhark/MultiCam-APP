// Panel side of MultiCam.Viewer: saved settings for PiP and wallpaper, size presets,
// one process per viewer and a manager that keeps the processes in line with the settings.
// C# 5 (csc of .NET Framework 4.x): no string interpolation, no ?. operator.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace MultiCam {
    public class PipSettings {
        public bool Enabled, TopMost, Locked;
        public int X, Y, Width, Height;
        public bool HasPosition;
    }

    public class WallpaperSettings {
        public bool Enabled;
        public string CameraId, Monitor, Fit = "fill";
        public int Fps = 30;
    }

    public static class ViewerSettings {
        static readonly int[] PresetWidths = { 320, 480, 640, 960 };

        static int HeightFor(int width, Mode mode) {
            int w = mode != null && mode.Width > 0 && mode.Height > 0 ? mode.Width : 16;
            int h = mode != null && mode.Width > 0 && mode.Height > 0 ? mode.Height : 9;
            return (int)Math.Round(width * (double)h / w, MidpointRounding.AwayFromZero);
        }

        public static List<Size> Presets(Mode mode) {
            return PresetWidths.Select(w => new Size(w, HeightFor(w, mode))).ToList();
        }

        // Fills missing parts and fixes impossible values. Never throws.
        public static void Normalize(Config config) {
            if (config == null) return;
            try {
                if (config.Cameras == null) config.Cameras = new List<Camera>();
                if (config.Wallpaper == null) config.Wallpaper = new WallpaperSettings();
                var wp = config.Wallpaper;
                if (wp.Fps != 30 && wp.Fps != 60) wp.Fps = 30;
                if (wp.Fit != "fill" && wp.Fit != "contain") wp.Fit = "fill";
                foreach (var c in config.Cameras) {
                    if (c == null) continue;
                    if (c.Pip == null) c.Pip = new PipSettings();
                    var p = c.Pip;
                    if (p.Width <= 0 || p.Height <= 0) { p.Width = 480; p.Height = HeightFor(480, c.Mode); }
                    if (Math.Abs((long)p.X) > 100000 || Math.Abs((long)p.Y) > 100000) p.HasPosition = false;
                }
            } catch { }
        }
    }

    public sealed class ViewerProcess : IDisposable {
        readonly Process process;
        readonly object gate = new object();
        readonly StringBuilder error = new StringBuilder();
        volatile bool ready;
        long hwnd;
        volatile int fps;

        public bool Ready { get { return ready; } }
        public long Hwnd { get { return System.Threading.Interlocked.Read(ref hwnd); } }
        public int Fps { get { return fps; } }
        public int ProcessId { get; private set; }
        public bool Exited { get { try { return process.HasExited; } catch { return true; } } }
        public int ExitCode { get { try { return process.HasExited ? process.ExitCode : 0; } catch { return -1; } } }
        public string Error { get { lock (gate) return error.ToString().Trim(); } }

        public event Action<int, int, int, int> GeometryChanged;
        public event Action<bool> LockChanged;
        public event Action<bool> TopMostChanged;

        static string CameraGuid(string id) { return new Guid(id).ToString("B").ToUpperInvariant(); }

        public static ViewerProcess StartPip(string viewerPath, Camera camera, int parentPid) {
            var p = camera.Pip;
            int x = p.X, y = p.Y;
            if (!p.HasPosition) {
                // First time: bottom-right corner of the main screen.
                var area = Screen.PrimaryScreen.WorkingArea;
                x = area.Right - p.Width - 24; y = area.Bottom - p.Height - 24;
            }
            string args = "--pip " + CameraGuid(camera.Id) + " " + x + " " + y + " " + p.Width + " " + p.Height + " " +
                (p.TopMost ? "1" : "0") + " " + (p.Locked ? "1" : "0") + " " + parentPid;
            return new ViewerProcess(viewerPath, args);
        }

        public static ViewerProcess StartWallpaper(string viewerPath, WallpaperSettings settings, int parentPid) {
            string args = "--wallpaper " + CameraGuid(settings.CameraId) + " " + Native.Quote(settings.Monitor) + " " +
                settings.Fps + " " + settings.Fit + " " + parentPid;
            return new ViewerProcess(viewerPath, args);
        }

        ViewerProcess(string path, string arguments) {
            process = new Process { StartInfo = new ProcessStartInfo(path, arguments) {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = System.IO.Path.GetDirectoryName(path)
            } };
            process.OutputDataReceived += (s, e) => { if (!String.IsNullOrWhiteSpace(e.Data)) OnEvent(e.Data); };
            process.ErrorDataReceived += (s, e) => { if (!String.IsNullOrWhiteSpace(e.Data)) lock (gate) error.AppendLine(e.Data); };
            process.Start();
            ProcessId = process.Id;
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
        }

        static int Int(Dictionary<string, object> e, string key) {
            object v; return e.TryGetValue(key, out v) && v != null ? Convert.ToInt32(v) : 0;
        }

        void OnEvent(string line) {
            Dictionary<string, object> e;
            try { e = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(line); } catch { return; }
            object name; if (e == null || !e.TryGetValue("Event", out name)) return;
            switch (name as string) {
                case "ready":
                    object h; if (e.TryGetValue("Hwnd", out h) && h != null) System.Threading.Interlocked.Exchange(ref hwnd, Convert.ToInt64(h));
                    ready = true; break;
                case "stats": fps = Int(e, "Fps"); break;
                case "geometry": { var g = GeometryChanged; if (g != null) g(Int(e, "X"), Int(e, "Y"), Int(e, "Width"), Int(e, "Height")); break; }
                case "lock": { var g = LockChanged; if (g != null) g(Int(e, "Value") != 0); break; }
                case "topmost": { var g = TopMostChanged; if (g != null) g(Int(e, "Value") != 0); break; }
            }
        }

        public void Send(string command) {
            try { if (!process.HasExited) process.StandardInput.WriteLine(command); } catch { }
        }

        public void Dispose() {
            try {
                if (!process.HasExited) {
                    Send("quit");
                    if (!process.WaitForExit(2000)) process.Kill();
                    process.WaitForExit(1000);
                }
            } catch { }
            try { process.Dispose(); } catch { }
        }
    }

    public sealed class ViewerManager : IDisposable {
        class Slot {
            public ViewerProcess Process;
            public int Failures;
            public DateTime RetryAt;
            public string Error;
            public bool Failed;          // gave up: Status shows the reason
            public string Waiting;       // monitor or desktop missing: tries again without giving up
            public DateTime StartedAt;
            // What the live process was told, to send only real changes.
            public bool TopMost, Locked, HasPosition;
            public int X, Y, Width, Height;
            public string Key;           // wallpaper: camera, monitor, fps, fit
        }

        readonly string viewerPath;
        readonly int parentPid;
        readonly Dictionary<string, Slot> pips = new Dictionary<string, Slot>();
        Slot wallpaper;
        readonly object gate = new object();
        // Changes reported by the viewers (reader threads), applied on the caller's thread by Reconcile.
        readonly List<Action> pending = new List<Action>();
        int started;

        public int Started { get { return started; } }

        public ViewerManager(string viewerPath, int parentPid) {
            this.viewerPath = viewerPath;
            this.parentPid = parentPid;
        }

        static readonly TimeSpan RetryDelay = TimeSpan.FromSeconds(2);
        static readonly TimeSpan WaitDelay = TimeSpan.FromSeconds(5);
        const int MaxFailures = 3;
        // A viewer that ran this long before failing starts a new count of failures.
        TimeSpan healthyAfter = TimeSpan.FromSeconds(30);
        public TimeSpan HealthyAfter { get { return healthyAfter; } set { healthyAfter = value; } }

        void Queue(Action change) { lock (gate) pending.Add(change); }

        static string Describe(ViewerProcess p) {
            string text = p.Error;
            if (String.IsNullOrEmpty(text)) text = "o visualizador encerrou (código " + p.ExitCode + ")";
            var lines = text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            return lines.Length > 0 ? lines[lines.Length - 1] : text;
        }

        // Returns true when the process ended for good (no restart).
        bool HandleExit(Slot slot) {
            var p = slot.Process; slot.Process = null;
            int code = p.ExitCode; string reason = Describe(p);
            bool healthy = p.Ready && DateTime.UtcNow - slot.StartedAt >= healthyAfter;
            p.Dispose();
            // 4: monitor missing (off, asleep, unplugged); 6: desktop not ready or Explorer restarted.
            if (code == 4 || code == 6) {
                slot.Waiting = code == 4 ? "Monitor ausente. Tentando de novo…" : "Área de trabalho indisponível. Tentando de novo…";
                slot.RetryAt = DateTime.UtcNow + WaitDelay;
                return false;
            }
            slot.Waiting = null;
            if (code == 5) { slot.Failed = true; slot.Error = reason; return true; }
            if (healthy) slot.Failures = 0;
            slot.Failures++;
            slot.Error = reason;
            Storage.Log("Visualizador: " + reason);
            if (slot.Failures >= MaxFailures) { slot.Failed = true; return true; }
            slot.RetryAt = DateTime.UtcNow + RetryDelay;
            return false;
        }

        void StartPip(Slot slot, Camera c) {
            var p = ViewerProcess.StartPip(viewerPath, c, parentPid);
            started++;
            slot.Process = p; slot.StartedAt = DateTime.UtcNow;
            slot.TopMost = c.Pip.TopMost; slot.Locked = c.Pip.Locked; slot.HasPosition = c.Pip.HasPosition;
            slot.X = c.Pip.X; slot.Y = c.Pip.Y; slot.Width = c.Pip.Width; slot.Height = c.Pip.Height;
            p.GeometryChanged += (x, y, w, h) => Queue(() => {
                c.Pip.X = x; c.Pip.Y = y; c.Pip.Width = w; c.Pip.Height = h; c.Pip.HasPosition = true;
                slot.X = x; slot.Y = y; slot.Width = w; slot.Height = h; slot.HasPosition = true;
            });
            p.LockChanged += v => Queue(() => { c.Pip.Locked = v; slot.Locked = v; });
            p.TopMostChanged += v => Queue(() => { c.Pip.TopMost = v; slot.TopMost = v; });
        }

        public bool Reconcile(Config config, Func<Camera, bool> cameraReady) {
            bool changed = false;
            List<Action> changes;
            lock (gate) { changes = new List<Action>(pending); pending.Clear(); }
            foreach (var change in changes) { change(); changed = true; }

            var ids = new HashSet<string>();
            foreach (var c in config.Cameras) {
                if (c == null || c.Pip == null || c.Id == null) continue;
                ids.Add(c.Id);
                bool want = c.Enabled && c.Pip.Enabled && cameraReady(c);
                Slot slot;
                pips.TryGetValue(c.Id, out slot);
                if (!want) {
                    if (slot != null) { if (slot.Process != null) slot.Process.Dispose(); pips.Remove(c.Id); }
                    continue;
                }
                if (slot == null) { slot = new Slot(); pips[c.Id] = slot; }
                if (slot.Process != null && slot.Process.Exited) {
                    if (slot.Process.ExitCode == 0) {
                        // Closed from its own menu ("Fechar PiP"): the user turned it off.
                        slot.Process.Dispose(); pips.Remove(c.Id);
                        c.Pip.Enabled = false; changed = true;
                        continue;
                    }
                    HandleExit(slot);
                }
                if (slot.Failed) continue;
                if (slot.Process == null) {
                    if (DateTime.UtcNow < slot.RetryAt) continue;
                    try { StartPip(slot, c); }
                    catch (Exception ex) { slot.Failed = true; slot.Error = ex.Message; }
                    continue;
                }
                var pip = c.Pip;
                if (pip.TopMost != slot.TopMost) { slot.Process.Send("topmost " + (pip.TopMost ? 1 : 0)); slot.TopMost = pip.TopMost; }
                if (pip.Locked != slot.Locked) { slot.Process.Send("lock " + (pip.Locked ? 1 : 0)); slot.Locked = pip.Locked; }
                if (pip.Width != slot.Width || pip.Height != slot.Height) {
                    slot.Process.Send("resize " + pip.Width + " " + pip.Height); slot.Width = pip.Width; slot.Height = pip.Height;
                }
                if (pip.HasPosition != slot.HasPosition || (pip.HasPosition && (pip.X != slot.X || pip.Y != slot.Y))) {
                    int x = pip.X, y = pip.Y;
                    if (!pip.HasPosition) {
                        var area = Screen.PrimaryScreen.WorkingArea;
                        x = area.Right - pip.Width - 24; y = area.Bottom - pip.Height - 24;
                    }
                    slot.Process.Send("move " + x + " " + y);
                    slot.HasPosition = pip.HasPosition; slot.X = pip.X; slot.Y = pip.Y;
                }
            }
            foreach (var gone in pips.Keys.Where(k => !ids.Contains(k)).ToList()) {
                if (pips[gone].Process != null) pips[gone].Process.Dispose();
                pips.Remove(gone);
            }

            ReconcileWallpaper(config, cameraReady);
            return changed;
        }

        void ReconcileWallpaper(Config config, Func<Camera, bool> cameraReady) {
            var w = config.Wallpaper;
            Camera camera = w == null || w.CameraId == null ? null : config.Cameras.FirstOrDefault(c => c != null && c.Id == w.CameraId);
            bool want = w != null && w.Enabled && camera != null && camera.Enabled && !String.IsNullOrEmpty(w.Monitor) && cameraReady(camera);
            if (!want) { StopWallpaper(); return; }
            string key = w.CameraId + "|" + w.Monitor + "|" + w.Fps + "|" + w.Fit;
            if (wallpaper != null && wallpaper.Key != key) StopWallpaper();
            if (wallpaper == null) wallpaper = new Slot { Key = key };
            if (wallpaper.Process != null && wallpaper.Process.Exited) HandleExit(wallpaper);
            if (wallpaper.Failed || wallpaper.Process != null || DateTime.UtcNow < wallpaper.RetryAt) return;
            try { wallpaper.Process = ViewerProcess.StartWallpaper(viewerPath, w, parentPid); wallpaper.StartedAt = DateTime.UtcNow; started++; }
            catch (Exception ex) { wallpaper.Failed = true; wallpaper.Error = ex.Message; }
        }

        void StopWallpaper() {
            if (wallpaper == null) return;
            if (wallpaper.Process != null) wallpaper.Process.Dispose();
            wallpaper = null;
        }

        static string StatusOf(Slot slot) {
            if (slot == null) return "";
            if (slot.Failed) return "Falhou: " + slot.Error;
            if (slot.Process != null && slot.Process.Ready) return "";
            if (slot.Waiting != null) return slot.Waiting;
            if (slot.Process == null) return slot.Failures > 0 ? "Reiniciando…" : "";
            return "Iniciando…";
        }

        public string Status(Camera camera) {
            Slot slot; pips.TryGetValue(camera.Id, out slot);
            return StatusOf(slot);
        }

        public string WallpaperStatus() { return StatusOf(wallpaper); }

        public int PipFps(Camera camera) {
            Slot slot; return pips.TryGetValue(camera.Id, out slot) && slot.Process != null ? slot.Process.Fps : 0;
        }
        public int WallpaperFps() { return wallpaper != null && wallpaper.Process != null ? wallpaper.Process.Fps : 0; }

        public void StopAll() {
            foreach (var slot in pips.Values) if (slot.Process != null) slot.Process.Dispose();
            pips.Clear();
            StopWallpaper();
        }

        public void Dispose() { StopAll(); }
    }
}
