"""Exercise the recovery workflow's real archive selection/extraction step."""
import os
from pathlib import Path
import json
import tempfile
import textwrap
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
workflow = (ROOT/'.github/workflows/prepare-recovery.yml').read_text()
marker = '          import json, os, pathlib, subprocess, zipfile\n'
start = workflow.index(marker)
code = compile(textwrap.dedent(workflow[start:workflow.index('\n          PY', start)]), 'recovery-workflow', 'exec')


class RecoveryBundleTests(unittest.TestCase):
    def exercise(self, kind):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)/'public-release';root.mkdir()
            (root/'firmware.bin').write_bytes(b'published firmware')
            bundle='kamiOS-v1.0.12.zip'
            names=[bundle] if kind in ('bundle','mismatch','missing_manifest') else ['latest.json'] if kind=='legacy' else []
            (root/'assets.json').write_text(json.dumps({'assets':[{'name':n} for n in names]}))
            if bundle in names:
                with zipfile.ZipFile(root/bundle,'w') as archive:
                    archive.writestr('firmware.bin', b'changed' if kind=='mismatch' else b'published firmware')
                    if kind!='missing_manifest':archive.writestr('latest.json',b'signed manifest')
            def download(*args,**kwargs):
                if kind=='legacy':(root/'latest.json').write_bytes(b'legacy manifest')
            previous=os.getcwd()
            try:
                os.chdir(temp)
                with patch.dict(os.environ,{'VERSION':'v1.0.12'}), patch('subprocess.run',side_effect=download):
                    exec(code,{})
                return (root/'latest.json').read_bytes()
            finally:os.chdir(previous)

    def test_new_bundle(self):
        self.assertEqual(self.exercise('bundle'),b'signed manifest')

    def test_old_release(self):
        self.assertEqual(self.exercise('legacy'),b'legacy manifest')

    def test_bad_or_missing_materials_fail_closed(self):
        for kind in ('mismatch','missing_manifest','absent'):
            with self.subTest(kind=kind),self.assertRaises(SystemExit):self.exercise(kind)
