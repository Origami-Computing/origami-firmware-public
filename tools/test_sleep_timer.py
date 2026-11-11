"""Compile and run production sleep-timer persistence tests without flashing.

Run with a host C++ compiler: python3 tools/test_sleep_timer.py --compiler g++
"""
import argparse
from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("g++") or shutil.which("clang++"))
    args = parser.parse_args()
    if not args.compiler:
        parser.error("Supply --compiler; no host C++ compiler found")
    root = Path(__file__).resolve().parents[1]
    output = root / ".cache" / "sleep-timer-tests"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ("sleep_timer.exe" if os.name == "nt" else "sleep_timer")
    subprocess.run([
        args.compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-O0", "-g",
        "-I", str(root / "include"), str(root / "test/test_sleep_timer/test_main.cpp"),
        str(root / "src/core/sleep_timer.cpp"), "-o", str(executable),
    ], check=True)
    with tempfile.TemporaryDirectory(prefix="state-", dir=output) as directory:
        def run(*arguments):
            subprocess.run([str(executable), *map(str, arguments)], cwd=directory, check=True)

        run("enable")
        run("expect", 60)  # Fresh NVS.
        for value in (120, 10, 3600, 60):
            run("save", value)
            run("expect", value)  # Separate process verifies persistence after reboot.
        for invalid in (0, 9, 3601, 0xFFFFFFFF):
            (Path(directory) / "sleep-state.bin").write_bytes(struct.pack("=I", invalid))
            run("expect", 60)
        run("failures")
        run("expect", 60)
    print("All 16 sleep-preference scenarios passed.")


if __name__ == "__main__":
    main()
