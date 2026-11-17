import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('release_notes', ROOT/'tools/release_notes.py')
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)
LOG = '# Release log\n\n## v1.0.12\n\nChanges since v1.0.11.\n\n- Fix gallery navigation when reaching the final photo.\n'


class ReleaseNotesTests(unittest.TestCase):
    def test_baseline_and_exact_version(self):
        body, baseline = notes.release_entry(LOG, 'v1.0.12', 'v1.0.11')
        self.assertEqual(baseline, 'v1.0.11')
        self.assertIn('Fix gallery', body)
        for version, previous in [('v1.0.13', None), ('v1.0.12', 'v1.0.10'), ('../../file', None)]:
            with self.subTest(version=version, previous=previous), self.assertRaises(ValueError):
                notes.release_entry(LOG, version, previous)

    def test_unfinished_duplicate_and_empty_notes_rejected(self):
        for log in [LOG + LOG, LOG.replace('Fix gallery', 'TODO gallery'),
                    LOG.split('- Fix')[0], LOG.replace('v1.0.11.', 'v1.0.12.'),
                    LOG + '\nThis source targets v1.0.6.\n']:
            with self.subTest(log=log), self.assertRaises(ValueError):
                notes.release_entry(log, 'v1.0.12')

    def test_only_requested_entry_is_rendered(self):
        body, _ = notes.release_entry(LOG + '\n## v1.0.11\n\nOld historical text.\n', 'v1.0.12')
        self.assertNotIn('historical', body)

    def test_bundle_preserves_assets_and_excludes_unrelated_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source = root/'dist'; source.mkdir(); output = root/'out'
            for name in notes.ASSETS:
                (source/name).write_bytes(('candidate bytes: '+name).encode())
            metadata = dict(version='v1.0.12', source_commit='a'*40,
                            firmware_sha256=hashlib.sha256((source/'firmware.bin').read_bytes()).hexdigest())
            (source/'REVIEW.json').write_text(json.dumps(metadata))
            (source/'unrelated.bin').write_bytes(b'never distribute')
            notes.package(source, output, 'v1.0.12', LOG)
            with zipfile.ZipFile(output/'kamiOS-v1.0.12.zip') as archive:
                self.assertEqual(set(archive.namelist()), set(notes.ASSETS)|{'RELEASE-NOTES.md', 'CHANGELOG.md'})
                for name in notes.ASSETS:
                    self.assertEqual(archive.read(name), (source/name).read_bytes())
                text = archive.read('RELEASE-NOTES.md').decode()
                self.assertIn('compare/v1.0.11...v1.0.12', text)
                self.assertIn('not a factory installer', text)
                self.assertIn('<details>', text)
            # Retrying packages deterministically named files, without extra ZIPs.
            notes.package(source, output, 'v1.0.12', LOG)
            self.assertEqual(len(list(output.glob('*.zip'))), 1)
            (source/'firmware.bin').write_bytes(b'tampered')
            with self.assertRaises(ValueError): notes.package(source, output, 'v1.0.12', LOG)
            (source/'LICENSE').unlink()
            with self.assertRaises(ValueError): notes.package(source, output, 'v1.0.12', LOG)

    def test_metadata_version_must_match(self):
        with self.assertRaises(ValueError):
            notes.render(LOG, 'v1.0.12', {'version': 'v1.0.6'})

    def test_workflow_keeps_verification_before_packaging_and_explicit_uploads(self):
        workflow = (ROOT/'.github/workflows/publish.yml').read_text()
        self.assertLess(workflow.index('tools/verify_release_candidate.py'), workflow.index('tools/release_notes.py --version "$VERSION" --directory dist'))
        self.assertIn('--previous-version "$PREVIOUS_VERSION"', workflow)
        self.assertIn('dist/firmware.bin "release-output/kamiOS-$VERSION.zip"', workflow)
        self.assertNotIn('dist/*', workflow)
        self.assertNotIn('RELEASE.txt', workflow)
