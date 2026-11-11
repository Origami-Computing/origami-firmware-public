"""Compile the production camera HAL with host SDK doubles and inject failures."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which("clang++") or shutil.which("g++")
if not compiler:
    raise SystemExit("Install a host C++ compiler (clang++ or g++).")
with tempfile.TemporaryDirectory(prefix="camera-startup-") as directory:
    binary = Path(directory) / "camera-startup"
    subprocess.run([
        compiler, "-std=c++11", "-Wall", "-Wextra",
        "-I", str(root / "test/camera_host/stubs"),
        "-I", str(root / "include"),
        str(root / "src/hal/hal_camera.cpp"),
        str(root / "test/camera_host/test_main.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
