"""Package the Qt GUI and already-built backend for the current OS/architecture.

Run on each target OS separately; PyInstaller is not a cross-compiler.
"""
import argparse
import importlib.util
from pathlib import Path
import platform
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build", help="Folder containing dsi2lsl and its copied runtime libraries")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    directory = Path(args.build_dir).resolve()
    sdk_directory = root / "DSI_API"
    cache = directory / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if line.startswith("DSI_API_DIR:PATH="):
                sdk_directory = Path(line.split("=", 1)[1])
                break
    if sys.platform == "win32":
        backend = directory / "dsi2lsl.exe"
        if not backend.exists() and (directory / "Release" / backend.name).exists():
            directory = directory / "Release"
            backend = directory / backend.name
        arch = "ARM64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"
        dsi = directory / f"libDSI-Windows-{arch}.dll"
        lsl = directory / "lsl.dll"
    elif sys.platform == "darwin":
        backend = directory / "dsi2lsl"
        dsi = directory / f"libDSI-Darwin-{platform.machine()}.dylib"
        lsl = directory / "liblsl.2.dylib"
    else:
        parser.error("This packaging helper targets macOS and Windows.")
    for module in ("PySide6", "PyInstaller"):
        if importlib.util.find_spec(module) is None:
            parser.error(f"Install {module} in this Python environment first.")
    for file in (backend, dsi, lsl):
        if not file.is_file():
            parser.error(f"Missing {file}; build the matching backend first, or set --build-dir.")
    # Version-load check is intentionally safe: it never connects to a real headset.
    subprocess.run([str(backend), "--check-libraries"], check=True, cwd=directory)
    command = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--windowed", "--onedir",
               "--name", "DSI2LSL", "--paths", str(root), "--distpath", str(root / "dist"),
               "--workpath", str(root / "build" / "gui-package" / "work"),
               "--specpath", str(root / "build" / "gui-package")]
    for file in (backend, dsi, lsl):
        command.extend(["--add-binary", f"{file}:backend"])
    for file, destination in ((root / "README.md", "docs"),
                              (root / "WINDOWS_BUILD.md", "docs"),
                              (root / "GUI" / "README.md", "docs/GUI"),
                              (sdk_directory / "LICENSE.md", "docs/vendor"),
                              (directory / "_deps" / "liblsl-src" / "LICENSE", "docs/liblsl")):
        if file.exists():
            command.extend(["--add-data", f"{file}:{destination}"])
    command.append(str(root / "GUI" / "launch.py"))
    subprocess.run(command, check=True, cwd=root)
    print("Built dist/DSI2LSL.app" if sys.platform == "darwin" else "Built dist/DSI2LSL/DSI2LSL.exe")


if __name__ == "__main__":
    main()
