"""Read-only structural audit of firmware MJPEG/PCM AVI files.

Require exactly one JPEG envelope per nonempty video chunk. This complements
full codec decoding, hashes and audio checks; it does not validate JPEG entropy,
physical A/V synchronization, timestamps, or AVI indexes. Never repairs media.
Exit 0: envelopes pass, 1: frame defects, 2: unsupported/truncated container.
"""
import argparse
import json
import mmap
from pathlib import Path
import struct


def jpeg_end(data, start, end):
    """Return the first complete envelope's end; skip length-delimited metadata."""
    if data[start:start + 2] != b'\xff\xd8':
        raise ValueError('missing JPEG SOI')
    at, scanning, saw_scan, saw_frame = start + 2, False, False, False
    while at < end:
        if scanning:
            while at < end and data[at] != 0xff:
                at += 1
        if at >= end or data[at] != 0xff:
            raise ValueError('missing JPEG marker')
        while at < end and data[at] == 0xff:
            at += 1
        if at == end:
            raise ValueError('truncated JPEG marker')
        marker = data[at]
        at += 1
        if scanning and (marker == 0 or 0xd0 <= marker <= 0xd7):
            continue  # Byte stuffing or restart marker inside entropy data.
        scanning = False
        if marker == 0xd9:
            if not saw_frame or not saw_scan:
                raise ValueError('JPEG ended without frame/scan headers')
            return at
        if marker in (0, 0xd8) or 0xd0 <= marker <= 0xd7:
            raise ValueError('unexpected JPEG standalone marker')
        if marker == 1:  # TEM, standalone.
            continue
        if marker < 0xc0 or at + 2 > end:
            raise ValueError('invalid or truncated JPEG segment')
        length = (data[at] << 8) | data[at + 1]
        if length < 2 or at + length > end:
            raise ValueError('JPEG segment exceeds frame chunk')
        if 0xc0 <= marker <= 0xcf and marker not in (0xc4, 0xc8, 0xcc):
            saw_frame = True
        if marker == 0xda:
            saw_scan = scanning = True
        at += length
    raise ValueError('missing JPEG EOI')


def chunks(data, start, end):
    while start < end:
        if end - start < 8:
            raise ValueError('truncated RIFF chunk header')
        size = struct.unpack_from('<I', data, start + 4)[0]
        body, stop = start + 8, start + 8 + size
        next_chunk = stop + (size & 1)
        if next_chunk > end:
            raise ValueError('RIFF chunk exceeds enclosing boundary')
        yield data[start:start + 4], body, stop
        start = next_chunk


def audit(data):
    if len(data) < 12 or data[:4] != b'RIFF' or data[8:12] != b'AVI ':
        raise ValueError('expected firmware RIFF AVI')
    if struct.unpack_from('<I', data, 4)[0] + 8 != len(data):
        raise ValueError('RIFF size does not match file length')
    report = dict(video_positions=0, empty_holds=0, jpeg_chunks=0,
                  defective_chunks=0, audio_bytes=0, maximum_jpeg_bytes=0,
                  examples=[])
    movies = 0
    for tag, body, stop in chunks(data, 12, len(data)):
        if tag != b'LIST':
            continue
        if stop - body < 4:
            raise ValueError('LIST missing type')
        if data[body:body + 4] != b'movi':
            continue
        movies += 1
        for kind, frame, end in chunks(data, body + 4, stop):
            size = end - frame
            if kind == b'01wb':
                if size % 2:
                    raise ValueError('partial PCM16 sample')
                report['audio_bytes'] += size
                continue
            if kind != b'00dc':
                raise ValueError('unsupported firmware movie chunk')
            index = report['video_positions']
            report['video_positions'] += 1
            if not size:
                report['empty_holds'] += 1
                continue
            report['jpeg_chunks'] += 1
            report['maximum_jpeg_bytes'] = max(report['maximum_jpeg_bytes'], size)
            try:
                consumed = jpeg_end(data, frame, end) - frame
                if consumed != size:
                    raise ValueError(f'{size - consumed} bytes after first JPEG EOI')
            except ValueError as error:
                report['defective_chunks'] += 1
                if len(report['examples']) < 20:
                    report['examples'].append(dict(position=index, offset=frame,
                                                    bytes=size, error=str(error)))
    if movies != 1 or not report['jpeg_chunks']:
        raise ValueError('expected one movie list with at least one JPEG')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('video', type=Path)
    args = parser.parse_args()
    try:
        with args.video.open('rb') as source:
            if source.seek(0, 2) == 0:
                raise ValueError('empty file')
            with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
                report = audit(data)
        print(json.dumps(report, indent=2))
        return int(bool(report['defective_chunks']))
    except (ValueError, OSError) as error:
        print(json.dumps({'error': str(error)}))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
