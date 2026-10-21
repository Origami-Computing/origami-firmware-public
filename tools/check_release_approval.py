"""Fail closed until a maintainer approves the exact tracked tree for publication."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
paths = sorted(filter(None, subprocess.check_output(
    ['git', 'ls-files', '-z'], cwd=root).decode().split('\0')))
digest = hashlib.sha256()
for name in paths:
    if name == 'release-approval.json':
        continue
    path = root / name
    if path.is_symlink():
        raise SystemExit('Symlink requires explicit review: ' + name)
    digest.update(name.encode() + b'\0' + hashlib.sha256(path.read_bytes()).digest())
actual = digest.hexdigest()
if '--digest' in sys.argv:
    print(actual)
    raise SystemExit(0)
approval = json.loads((root / 'release-approval.json').read_text())
required = ('approved', 'source_review_complete', 'license_review_complete', 'security_contact_ready')
if not all(approval.get(key) is True for key in required):
    raise SystemExit('BLOCKED: source, rights and security review are incomplete')
if not approval.get('approved_by') or approval.get('reviewed_tree_sha256') != actual:
    raise SystemExit('BLOCKED: approval must identify reviewer and exact tracked tree digest')
print('PASS source approval for publication; binary still requires separate review')
