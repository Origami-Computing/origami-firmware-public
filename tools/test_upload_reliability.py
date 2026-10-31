"""Repeatable host gate for upload transport, queue observations and resource ownership.

No hardware, account credentials or network requests. Device/cloud verification
is separate in upload_reliability_device_check.py and the backend e2e suite.
"""
from pathlib import Path
import argparse, subprocess, sys
ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--thread-sanitizer', action='store_true')
args = p.parse_args()
checks = [
    ('test_upload_backend.py', []),
    ('test_upload_receipt.py', []),
    ('test_upload_counts.py', []),
    ('test_upload_retry.py', []),
    ('test_upload_manifest.py', []),
    ('test_upload_fixture.py', []),
    ('test_upload_fixture_serial.py', []),
    ('test_upload_suspension.py', []),
    ('test_background_network.py', []),
    ('probe_upload_worker_lifecycle.py', []),
    ('test_background_restart.py', []),
    ('test_network_stack.py', []),
    ('test_agent_slab.py', []),
    ('test_camera_session.py', []),
    ('test_microphone_capture.py', []),
    ('test_peripheral_reservation.py', []),
    ('test_firmware_update_session.py', []),
    ('test_hardware_ci_runner.py', []),
    ('check_architecture_dependencies.py', []),
]
if args.thread_sanitizer:
    checks.append(('probe_upload_worker_lifecycle.py', ['--thread-sanitizer']))
for script, options in checks:
    print('RUN', script, *options, flush=True)
    subprocess.run([sys.executable, str(ROOT/'tools'/script), *options], cwd=ROOT, check=True)
print('PASS upload reliability host gate; live device/backend checks remain separate')
