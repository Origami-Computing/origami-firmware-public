"""Reject known internal station tooling and local data in public distributions.

This path policy complements secret scanning; it does not inspect file contents
or prove that renamed tools, binary credentials, or Git history are safe.
"""
import argparse
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
INTERNAL_FILES = {
    'firmware tool.cmd', 'scripts/firmware_tool.py',
    'scripts/firmware_device.py', 'scripts/firmware_log.py',
    'scripts/launch_firmware_tool.py', 'scripts/install_firmware_shortcut.ps1',
    'scripts/station_releases.py', 'scripts/ota_provisioning.py',
    'scripts/bulk_flash.py', 'scripts/bulk_flash_gui.py',
    'scripts/export_bulk_flash.py', 'scripts/provision_device.py',
    'scripts/provision_device.cmd', 'scripts/provision_device.sh',
    'scripts/tests/test_firmware_tool.py', 'scripts/tests/test_firmware_browser.js',
    'scripts/tests/test_bulk_flash.py', 'tools/ota/access_keys.py',
    'tools/ota/build_installers.py', 'docs/firmware-tool.md',
}
LOCAL_NAMES = {'.firmware-tool.json', '.ota-provisioning.json'}

def check_paths(paths):
    failures = []
    for original in paths:
        name = original.replace('\\', '/').casefold()
        parts = [p for p in name.split('/') if p and p != '.']
        if '..' in parts or name.startswith('/') or (parts and ':' in parts[0]):
            failures.append(original)
            continue
        if parts and parts[0] == 'application':
            parts = parts[1:]
        name = '/'.join(parts)
        if (name in INTERNAL_FILES or name.startswith('scripts/firmware-tool/')
                or any(p in LOCAL_NAMES or p in {'.origami', '.pio', '.git'} for p in parts)):
            failures.append(original)
    if failures:
        raise ValueError('Internal/local paths in public distribution:\n' + '\n'.join(failures))

def check_archive(path):
    with tarfile.open(path, 'r:*') as archive:
        for entry in archive:
            check_paths([entry.name])
            if not (entry.isfile() or entry.isdir()):
                raise ValueError('Unsupported source archive entry: ' + entry.name)

def check_tracked():
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    check_paths(filter(None, names))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    try:
        if args.archive:
            check_archive(args.archive)
        else:
            check_tracked()
    except (ValueError, tarfile.TarError) as error:
        raise SystemExit(str(error))
    print('PASS public distribution path policy')
