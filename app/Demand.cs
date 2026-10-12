// Capture on demand, panel side: reads the demand slots written by the clients of a camera
// (native/demand.hpp), wakes the panel when a client starts, and decides when a card runs.
// C# 5 (csc of .NET Framework 4.x): no string interpolation, no ?. operator.
using System;
using System.Collections.Generic;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Threading;

namespace MultiCam {
    public static class Demand {
        // Layout of native/demand.hpp: 16-byte header, then 16 slots of {owner, pid, tick}.
        const uint Magic = 0x444D434D;
        const int Slots = 16, SlotSize = 16, HeaderSize = 16, Size = HeaderSize + Slots * SlotSize;
        const ulong FreshMs = 3000;

        [DllImport("kernel32.dll")] static extern ulong GetTickCount64();
        public static ulong Now() { return GetTickCount64(); }

        static string Name(string cameraId) {
            return "Local\\MultiCam-" + new Guid(cameraId).ToString("B").ToUpperInvariant() + "-demand";
        }

        public static bool Active(string cameraId) { return Active(cameraId, Now()); }

        // True when some client holds a slot refreshed less than 3 s ago. No area, no demand.
        public static bool Active(string cameraId, ulong nowTick) {
            try {
                using (var file = MemoryMappedFile.OpenExisting(Name(cameraId), MemoryMappedFileRights.Read))
                using (var view = file.CreateViewAccessor(0, Size, MemoryMappedFileAccess.Read)) {
                    if (view.ReadUInt32(0) != Magic) return false;
                    for (int i = 0; i < Slots; i++) {
                        long slot = HeaderSize + i * SlotSize;
                        if (view.ReadUInt32(slot) != 0 && nowTick - view.ReadUInt64(slot + 8) < FreshMs) return true;
                    }
                }
            } catch { }
            return false;
        }
    }

    // Calls `wake` (on a background thread) whenever a client takes a demand slot.
    public sealed class DemandWaker : IDisposable {
        readonly EventWaitHandle signal = new EventWaitHandle(false, EventResetMode.AutoReset, "Local\\MultiCam-wake");
        readonly Thread thread;
        volatile bool stopping;

        public DemandWaker(Action wake) {
            thread = new Thread(() => {
                while (!stopping) {
                    if (signal.WaitOne(1000) && !stopping) { try { wake(); } catch { } }
                }
            }) { IsBackground = true, Name = "MultiCam demand waker" };
            thread.Start();
        }

        public void Dispose() {
            stopping = true;
            thread.Join(2000);
            signal.Dispose();
        }
    }

    public enum CaptureAction { Keep, Start, Stop }

    // When a card should capture: while its camera is in use and for Grace after the last use.
    // Pure logic with the clock passed in; the panel applies the decision.
    public sealed class CaptureScheduler {
        public static readonly TimeSpan Grace = TimeSpan.FromSeconds(60);
        readonly Dictionary<string, DateTime> lastUse = new Dictionary<string, DateTime>();

        // validated: the virtual camera is registered (the mode was checked with real frames).
        public CaptureAction Decide(string id, bool validated, bool demand, bool running, DateTime now) {
            if (!validated) return running ? CaptureAction.Keep : CaptureAction.Start;
            if (demand) { lastUse[id] = now; return running ? CaptureAction.Keep : CaptureAction.Start; }
            DateTime last;
            bool recent = lastUse.TryGetValue(id, out last) && now - last < Grace;
            if (recent) return CaptureAction.Keep;
            return running ? CaptureAction.Stop : CaptureAction.Keep;
        }

        // Validation (first real frames) counts as a use, so a new card runs for Grace.
        public void Touch(string id, DateTime now) { lastUse[id] = now; }

        public void Forget(string id) { lastUse.Remove(id); }
    }
}
