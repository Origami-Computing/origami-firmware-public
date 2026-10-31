"""Regress the acceptance gap where joined JPEGs still decode successfully."""
import struct
import unittest

from audit_video_frames import audit, jpeg_end


def segment(marker, payload):
    return bytes((255, marker)) + struct.pack('>H', len(payload) + 2) + payload


def jpeg(metadata=b'', entropy=b'\x10\xff\x00\x20\xff\xd0\x30'):
    return (b'\xff\xd8' + segment(0xe1, metadata) +
            segment(0xc0, b'\x08\x00\xf0\x01\x40\x01\x01\x11\x00') +
            segment(0xda, b'\x01\x01\x00\x00\x3f\x00') + entropy + b'\xff\xd9')


def chunk(tag, body):
    return tag + struct.pack('<I', len(body)) + body + (b'\0' if len(body) % 2 else b'')


def avi(frames):
    movie = b''.join(chunk(b'00dc', frame) for frame in frames)
    body = b'AVI ' + chunk(b'LIST', b'movi' + movie + chunk(b'01wb', b'\0' * 24))
    return b'RIFF' + struct.pack('<I', len(body)) + body


class FrameAudit(unittest.TestCase):
    def test_metadata_markers_are_not_extra_images(self):
        image = jpeg(b'\xff\xd8\xff\xd9\xff\xda')
        self.assertEqual(jpeg_end(image, 0, len(image)), len(image))
        result = audit(avi([image, b'', image]))
        self.assertEqual(result['defective_chunks'], 0)
        self.assertEqual(result['video_positions'], 3)
        self.assertEqual(result['empty_holds'], 1)
        self.assertEqual(result['audio_bytes'], 24)

    def test_joined_images_and_trailing_padding_fail(self):
        image = jpeg()
        for tail in (image, b'\0' * 13 + image, b'\0'):
            with self.subTest(tail=tail):
                report = audit(avi([image + tail]))
                self.assertEqual(report['defective_chunks'], 1)
                self.assertIn('after first JPEG EOI', report['examples'][0]['error'])

    def test_truncation_and_nested_soi_fail(self):
        image = jpeg()
        for incomplete in (image[:-2], image[:-1], image[:8],
                           jpeg(entropy=b'\xff\xd8'), b'\xff\xd8\xff\xd9'):
            self.assertEqual(audit(avi([incomplete]))['defective_chunks'], 1)

    def test_segment_length_cannot_escape_chunk(self):
        image = bytearray(jpeg())
        image[4:6] = b'\xff\xff'
        self.assertEqual(audit(avi([image, jpeg()]))['defective_chunks'], 1)

    def test_container_boundaries(self):
        data = avi([jpeg()])
        for bad in (b'', data[:-1], data + b'\0'):
            with self.assertRaises(ValueError):
                audit(bad)
        bad = bytearray(data)
        bad[28:32] = struct.pack('<I', len(data))  # First JPEG chunk length.
        with self.assertRaises(ValueError):
            audit(bad)

    def test_bounded_diagnostics(self):
        report = audit(avi([jpeg() + jpeg()] * 100))
        self.assertEqual(report['defective_chunks'], 100)
        self.assertEqual(len(report['examples']), 20)


if __name__ == '__main__':
    unittest.main()
