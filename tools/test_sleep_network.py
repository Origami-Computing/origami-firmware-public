"""Compatibility entry point for current radio-drain and sleep-boundary checks.

The old upload_manager sleep owner was replaced by RadioPause. Run the real
current owner and its power-transition caller instead of extracting retired code.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
for name in ('test_background_network.py', 'test_sleep_network_boundary.py'):
    subprocess.run([sys.executable, str(ROOT / 'tools' / name)], check=True)
