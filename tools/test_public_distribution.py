"""Exercise source/archive exclusion without firmware builds or credentials."""
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from check_public_distribution import check_archive, check_paths

class DistributionTests(unittest.TestCase):
    def test_public_build_and_firmware_remain_allowed(self):
        check_paths(['src/services/manufacturing.cpp', 'scripts/sd_write_compat.py',
                     'tools/ota/publish.py', 'application/LICENSE',
                     'framework/tools/sdk/esp32s3/lib/libdriver.a'])

    def test_internal_and_local_paths(self):
        for path in ['scripts/firmware_tool.py', 'scripts/firmware-tool/app.js',
                     'scripts/station_releases.py', '.firmware-tool.json',
                     '.ota-provisioning.json', '.origami/private/backup.bin',
                     '.pio/firmware-tool/commands.log', 'Firmware Tool.cmd']:
            for prefix in ['', 'application/']:
                with self.subTest(path=prefix+path), self.assertRaises(ValueError):
                    check_paths([prefix+path])

    def test_normalization_and_traversal(self):
        for path in ['application\\scripts\\FIRMWARE_TOOL.PY',
                     './application/.firmware-tool.json', '../secret', '/secret']:
            with self.subTest(path=path), self.assertRaises(ValueError):
                check_paths([path])

    def test_actual_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'source.tar.gz'
            for name, blocked in [('application/LICENSE', False),
                                  ('application/scripts/firmware_tool.py', True)]:
                with tarfile.open(path, 'w:gz') as archive:
                    entry = tarfile.TarInfo(name); entry.size = 4
                    archive.addfile(entry, io.BytesIO(b'test'))
                if blocked:
                    with self.assertRaises(ValueError): check_archive(path)
                else:
                    check_archive(path)
            with tarfile.open(path, 'w:gz') as archive:
                entry = tarfile.TarInfo('application/link')
                entry.type = tarfile.SYMTYPE; entry.linkname = '/private'
                archive.addfile(entry)
            with self.assertRaises(ValueError): check_archive(path)

if __name__ == '__main__':
    unittest.main()
