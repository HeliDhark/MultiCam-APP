"""Package binaries with rebuildable sources; exclude machine-specific profiles."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

root = Path(__file__).resolve().parents[1]
target = root / "artifacts" / "MultiCam-Windows10.zip"
target.parent.mkdir(exist_ok=True)
with ZipFile(target, "w", ZIP_DEFLATED) as archive:
    for path in (root / "dist").rglob("*"):
        relative = path.relative_to(root / "dist")
        if path.is_file() and "data" not in relative.parts:
            archive.write(path, str(Path("MultiCam") / relative))
    for folder in ("app", "native", "tests", "third_party"):
        for path in (root / folder).rglob("*"):
            relative = path.relative_to(root)
            if path.is_file() and ".git" not in relative.parts and "__pycache__" not in relative.parts:
                archive.write(path, str(Path("MultiCam/source") / relative))
    for name in ("CMakeLists.txt", "build.ps1", "README.md", "THIRD_PARTY.md", "VALIDATION.md", "LICENSE", ".gitignore"):
        archive.write(root / name, str(Path("MultiCam/source") / name))
with ZipFile(target) as archive:
    assert archive.testzip() is None
    assert "MultiCam/MultiCam.exe" in archive.namelist()
    assert "MultiCam/source/third_party/libdshowcapture/source/output-filter.cpp" in archive.namelist()
print(f"{target} ({target.stat().st_size:,} bytes)")
