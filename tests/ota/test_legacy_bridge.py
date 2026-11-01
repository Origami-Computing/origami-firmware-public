from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/ota'))
import legacy_bridge


class LegacyBridgeTests(unittest.TestCase):
    def setUp(self):
        self.image = bytes([0xe9])+bytes(31)+bytes.fromhex('3254cdab')+bytes(256)+b'v1.2.3\0'
        self.public = legacy_bridge.publish.manifest_payload(
            self.image, 'public', 'v1.2.3', 'n16r8-rev4', 'n16r8-ota-v1')

    def prepare(self, public=None, image=None):
        with patch.object(legacy_bridge.publish, 'verify_manifest', return_value=public or self.public):
            return legacy_bridge.payloads(image or self.image, {})

    def test_legacy_channel_version_contract_and_identical_image(self):
        bridges = self.prepare()
        for channel, payload in bridges.items():
            parsed = legacy_bridge.publish.version_tuple(payload['version'])
            self.assertEqual(channel == 'staging', parsed[3] >= 0)
            self.assertEqual(payload['channel'], channel)
            self.assertTrue(payload['path'].startswith('/releases/'+channel+'/n16r8-rev4/n16r8-ota-v1/'))
            for field in ('size', 'sha256', 'hardware', 'layout'):
                self.assertEqual(payload[field], self.public[field])
        self.assertEqual(self.public['version'], 'v1.2.3')

    def test_private_or_wrong_hardware_cannot_be_bridged(self):
        for field, value in [('channel','private'), ('hardware','other-board'), ('path','/private.bin')]:
            with self.assertRaises(ValueError): self.prepare(dict(self.public, **{field:value}))

    def test_changed_bytes_cannot_be_bridged(self):
        with self.assertRaises(ValueError): self.prepare(image=self.image+b'changed')

    def test_signature_failure_is_not_bypassed(self):
        with patch.object(legacy_bridge.publish, 'verify_manifest', side_effect=ValueError('signature')):
            with self.assertRaises(ValueError): legacy_bridge.payloads(self.image, {})
