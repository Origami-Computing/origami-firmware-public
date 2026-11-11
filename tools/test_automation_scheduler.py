"""Run the real scheduler and vibration owner with fake time, storage and I/O.

Covers calendar arithmetic, alert state, queueing and ownership; no network,
physical button, motor timing or rendering verification is implied.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="kami-scheduler-") as directory:
    binary = Path(directory) / "scheduler"
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra",
        "-fsanitize=address,undefined",
        "-I" + str(ROOT / "test/morning_host/stubs"),
        "-I" + str(ROOT / "include"),
        str(ROOT / "test/morning_host/test_main.cpp"),
        str(ROOT / "src/services/automation.cpp"),
        str(ROOT / "src/core/alert_vibration.cpp"),
        str(ROOT / "src/services/vibration.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
