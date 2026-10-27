import base64
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('publish', ROOT/'tools/ota/publish.py')
publish = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish)


def image():
    return (bytes([0xe9]) + bytes(31) + bytes.fromhex('3254cdab') + bytes(256) +
            b'v0.1.195-staging.171\0v0.1.194-staging.170\0v1.2.3\0')


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.access = patch.object(publish, 'access_key', return_value='a'*64)
        self.access.start()
        self.addCleanup(self.access.stop)

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.key = Path(cls.tmp.name)/'key.pem'
        cls.pub = Path(cls.tmp.name)/'pub.pem'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048',
                        '-out', str(cls.key)], check=True, capture_output=True)
        cls.pub.write_bytes(subprocess.check_output(['openssl', 'pkey', '-in', str(cls.key), '-pubout']))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def payload(self):
        return publish.manifest_payload(image(), 'staging', 'v0.1.195-staging.171', 'n16r8-rev4', 'n16r8-ota-v1')

    def signed(self):
        raw = json.dumps(self.payload(), sort_keys=True, separators=(',', ':')).encode()
        sig = subprocess.check_output(['openssl', 'dgst', '-sha256', '-sign', str(self.key)], input=raw)
        return dict(payload=raw.decode(), signature=base64.b64encode(sig).decode())

    def test_roundtrip_and_tampered_metadata(self):
        envelope = self.signed()
        self.assertEqual(publish.verify_manifest(envelope, self.pub), self.payload())
        envelope['payload'] = envelope['payload'].replace('staging', 'public')
        with self.assertRaises(subprocess.CalledProcessError):
            publish.verify_manifest(envelope, self.pub)

    def test_wrong_key_rejected(self):
        with self.assertRaises(subprocess.CalledProcessError):
            publish.verify_manifest(self.signed())

    def test_channels_cannot_mix(self):
        for channel, version in [('public', 'v1.0.0-staging.12'), ('staging', 'v1.0.0')]:
            with self.assertRaises(ValueError):
                publish.manifest_payload(image(), channel, version, 'n16r8-rev4', 'n16r8-ota-v1')

    def test_merged_or_oversized_images_rejected(self):
        for data in (b'\xff'*100, b'\xe9'+bytes(100), image()+bytes(publish.MAX_IMAGE_SIZE)):
            with self.assertRaises(ValueError):
                publish.validate_image(data)

    def test_embedded_credential_rejected(self):
        for token in (b'github_pat_'+b'a'*40, b'ghp_'+b'B'*36):
            with self.assertRaises(ValueError):
                publish.validate_image(image()+token)

    def test_mislabeled_firmware_version_rejected(self):
        with self.assertRaisesRegex(ValueError, 'not embedded'):
            publish.manifest_payload(image(), 'public', 'v9.9.9', 'n16r8-rev4', 'n16r8-ota-v1')

    def test_image_change_changes_hash_and_path(self):
        before = self.payload()
        after = publish.manifest_payload(image()+b'x', 'staging', before['version'], before['hardware'], before['layout'])
        self.assertNotEqual(before['sha256'], after['sha256'])
        self.assertNotEqual(before['path'], after['path'])

    def test_invalid_identifiers_and_versions(self):
        for version in ['latest', 'v1.2.3/../../x', 'v-1.2.3', 'v1.2.3-staging.-1']:
            with self.assertRaises(ValueError): publish.version_tuple(version)
        with self.assertRaises(ValueError):
            publish.manifest_payload(image(), 'public', 'v1.2.3', '../wrong', 'layout')

    def test_channel_pointer_not_written_when_public_bytes_fail_verification(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as tmp:
            firmware = Path(tmp)/'firmware.bin'
            firmware.write_bytes(image())
            args = SimpleNamespace(firmware=firmware, channel='staging',
                version='v0.1.195-staging.171', hardware='n16r8-rev4', layout='n16r8-ota-v1',
                signing_key=self.key, output=Path(tmp)/'out', prepare_only=False,
                bucket='firmware-bucket', base_url='https://example.cloudfront.net')
            with patch.object(publish, 'sign_manifest', return_value=self.signed()), \
                 patch.object(publish, 'aws', side_effect=[b'{}', b'{}']) as aws, \
                 patch.object(publish, 'fetch', return_value=b'tampered'):
                with self.assertRaisesRegex(ValueError, 'did not match'):
                    publish.publish(args)
                writes = [call.args for call in aws.call_args_list if 'put-object' in call.args]
                self.assertEqual(len(writes), 1)
                self.assertTrue(writes[0][writes[0].index('--key')+1].startswith('releases/'))

    def test_old_or_replaced_release_rejected_before_upload(self):
        from types import SimpleNamespace
        for version, content in [('v0.1.194-staging.170', image()),
                                 ('v0.1.195-staging.171', image()+b'changed')]:
            with self.subTest(version=version), tempfile.TemporaryDirectory() as tmp:
                firmware = Path(tmp)/'firmware.bin'
                firmware.write_bytes(content)
                args = SimpleNamespace(firmware=firmware, channel='staging', version=version,
                    hardware='n16r8-rev4', layout='n16r8-ota-v1', signing_key=self.key,
                    output=Path(tmp)/'out', prepare_only=False,
                    bucket='firmware-bucket', base_url='https://example.cloudfront.net')
                def fake_aws(*arguments):
                    if arguments[1] == 'list-objects-v2':
                        return json.dumps({'Contents':[{'Key':'channels/staging/n16r8-rev4/n16r8-ota-v1.json'}]}).encode()
                    if arguments[1] == 'get-object':
                        Path(arguments[-1]).write_text(json.dumps(self.signed()))
                        return b'{}'
                    self.fail('Must not upload when replacing or downgrading a release')
                with patch.object(publish, 'aws', side_effect=fake_aws), \
                     patch.object(publish, 'sign_manifest', return_value=self.signed()), \
                     patch.object(publish, 'verify_manifest', return_value=self.payload()):
                    with self.assertRaises(ValueError): publish.publish(args)


class AccessTransportTests(unittest.TestCase):
    def test_missing_or_malformed_key_fails_closed(self):
        for value in ('', 'short', 'A'*64):
            with patch.dict(publish.os.environ, {'OTA_ACCESS_KEY': value}):
                with self.assertRaises(ValueError): publish.access_key()
        with patch.dict(publish.os.environ, {'OTA_ACCESS_KEY': 'a'*64}):
            self.assertEqual(publish.access_key(), 'a'*64)

    def test_header_only_and_no_redirect_following(self):
        with patch.object(publish.urllib.request, 'build_opener') as opener:
            opener.return_value.open.return_value.__enter__.return_value.read.return_value = b'firmware'
            self.assertEqual(publish.fetch('https://example.cloudfront.net/releases/production/image', 'a'*64), b'firmware')
            request = opener.return_value.open.call_args.args[0]
            self.assertEqual(request.get_header('Authorization'), 'Bearer '+'a'*64)
            self.assertNotIn('a'*64, request.full_url)
            self.assertIsInstance(opener.call_args.args[0], publish.NoRedirect)
        self.assertIsNone(publish.NoRedirect().redirect_request(None,None,302,'',{},'https://elsewhere.invalid'))


if __name__ == '__main__':
    unittest.main()
