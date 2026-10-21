"""Reject embedded credentials without printing their values.

Checks tracked working-tree text, not Git history or ignored local files.
Public CA certificates, public signing keys and synthetic test credentials remain
valid inputs. Pattern scanning cannot prove the absence of every possible secret.
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
RULES = {
    'private key': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----'),
    'provider token': re.compile(r'\b(?:sk-[A-Za-z0-9_-]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[A-Z0-9]{16})\b'),
    'JWT credential': re.compile(r'\beyJ[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}\b'),
    'embedded firmware credential': re.compile(r'^\s*#\s*define\s+(?:WIFI_SSID|WIFI_PASS|AUTH_TOKEN|ORIGAMI_WIFI_SSID|ORIGAMI_WIFI_PASS|ORIGAMI_AUTH_TOKEN)\s+"[^"\n]+"'),
}
failures = []
paths = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
for name in filter(None, paths):
    path = ROOT / name
    if not path.is_file():
        continue
    if (path.name == '.env' or path.name.startswith('.env.')) and path.name != '.env.example':
        failures.append((name, 0, 'tracked environment secrets file'))
    if name == 'include/config/secrets.h':
        failures.append((name, 0, 'tracked local secrets header'))
    try:
        source = path.read_text()
    except (UnicodeError, OSError):
        continue
    for number, line in enumerate(source.splitlines(), 1):
        for label, pattern in RULES.items():
            if label == "embedded firmware credential" and name.startswith("tools/test_"):
                continue  # Synthetic host-test fixtures; token/private-key rules still apply.
            if pattern.search(line):
                failures.append((name, number, label))
for name, line, label in failures:
    print(f'{name}:{line}: {label} (value suppressed)')
if failures:
    sys.exit(1)
print('PASS tracked-text credential patterns and empty firmware defaults')
