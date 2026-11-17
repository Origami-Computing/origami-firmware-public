import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('fetch_candidate',ROOT/'tools/fetch_release_candidate.py')
fetch=importlib.util.module_from_spec(spec);spec.loader.exec_module(fetch)


class ArtifactTests(unittest.TestCase):
    def test_unique_retained_candidate(self):
        artifact={'name':'candidate-v1.0.13-public-review.1','expired':False,'id':12}
        self.assertEqual(fetch.select_artifact([{'artifacts':[artifact]}],'v1.0.13-public-review.1')['id'],12)
        for items in ([],[dict(artifact,expired=True)],[artifact,artifact]):
            with self.assertRaises(ValueError):fetch.select_artifact([{'artifacts':items}],'v1.0.13-public-review.1')

    def test_trusted_run_only(self):
        run=dict(path='.github/workflows/release.yml',head_branch='main',event='workflow_dispatch',conclusion='success')
        fetch.validate_run(run)
        for key in run:
            with self.subTest(key=key),self.assertRaises(ValueError):fetch.validate_run(dict(run,**{key:'other'}))

    def test_unpack_exact_assets_only(self):
        def archive(names):
            data=io.BytesIO()
            with zipfile.ZipFile(data,'w') as z:
                for name in names:z.writestr(name,b'verified later')
            return data.getvalue()
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            fetch.unpack(archive(fetch.ASSETS),root/'valid')
            self.assertEqual({p.name for p in (root/'valid').iterdir()},fetch.ASSETS)
            with self.assertRaises(ValueError):fetch.unpack(archive(fetch.ASSETS),root/'valid')
            for names in (fetch.ASSETS|{'../unexpected'},fetch.ASSETS-{'firmware.bin'}):
                with self.assertRaises(ValueError):fetch.unpack(archive(names),root/'bad')
