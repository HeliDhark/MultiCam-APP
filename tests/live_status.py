"""Read only frame counters from running MultiCam sessions; never read video."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
k = c.WinDLL("kernel32", use_last_error=True)
for name, args, result in (
    ("OpenFileMappingW", [w.DWORD, w.BOOL, w.LPCWSTR], w.HANDLE),
    ("MapViewOfFile", [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, c.c_size_t], c.c_void_p),
    ("OpenMutexW", [w.DWORD, w.BOOL, w.LPCWSTR], w.HANDLE),
    ("WaitForSingleObject", [w.HANDLE, w.DWORD], w.DWORD),
    ("ReleaseMutex", [w.HANDLE], w.BOOL),
    ("UnmapViewOfFile", [c.c_void_p], w.BOOL),
    ("CloseHandle", [w.HANDLE], w.BOOL),
    ("GetTickCount64", [], c.c_ulonglong),
):
    func = getattr(k, name)
    func.argtypes, func.restype = args, result


def snapshot(camera):
    name = "Local\\MultiCam-{" + camera["Id"].upper() + "}"
    mapping = k.OpenFileMappingW(4, False, name + "-frames")
    view = mutex = None
    try:
        if not mapping:
            return None
        view = k.MapViewOfFile(mapping, 4, 0, 0, 48)
        mutex = k.OpenMutexW(0x100001, False, name + "-lock")
        if not view or not mutex or k.WaitForSingleObject(mutex, 100) not in (0, 128):
            return None
        try:
            header = struct.unpack("<IIIIqQQII", c.string_at(view, 48))
            stamp = time.perf_counter()
        finally:
            k.ReleaseMutex(mutex)
        if header[0] != 0x4D434D31:
            return None
        return {"time": stamp, "width": header[1], "height": header[2],
                "interval": header[4], "frames": header[5],
                "age_ms": k.GetTickCount64() - header[6], "active": bool(header[7])}
    finally:
        if mutex:
            k.CloseHandle(mutex)
        if view:
            k.UnmapViewOfFile(view)
        if mapping:
            k.CloseHandle(mapping)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        parser.error("seconds must be between 1 and 60")
    cameras = json.loads((ROOT / "dist/data/settings.json").read_text(encoding="utf-8-sig"))["Cameras"]
    first = [snapshot(camera) for camera in cameras]
    time.sleep(args.seconds)
    rows = []
    for camera, before in zip(cameras, first):
        after = snapshot(camera)
        row = {"name": camera["Alias"], "device": camera["LastName"], "id": camera["Id"],
               "configured": camera["Mode"], "connected": after is not None}
        if after:
            row.update(active=after["active"], frame_age_ms=after["age_ms"])
        if before and after and after["frames"] >= before["frames"]:
            elapsed = after["time"] - before["time"]
            row.update(seconds=round(elapsed, 3), frames=after["frames"] - before["frames"],
                       fps=round((after["frames"] - before["frames"]) / elapsed, 2))
        rows.append(row)
    report = json.dumps(rows, indent=2, ensure_ascii=False)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report + "\n", encoding="utf-8")
    print(report)


if __name__ == "__main__":
    main()
