"""Bounded SDPUT framing for synthetic upload fixtures; no control commands mid-file."""
import re
import time
import zlib

class FixtureTransferError(RuntimeError):
    def __init__(self, message, serial_ready):
        super().__init__(message)
        self.serial_ready = serial_ready

def put_fixture(device, path, data):
    if not re.fullmatch(r'(?:/upload_queue/test-[A-Za-z0-9_-]+\.(?:jpg|wav|avi)|/videos/test-[A-Za-z0-9_-]+\.avi)', path):
        raise ValueError('Requires an isolated synthetic upload fixture path')
    transcript = ''
    def read(seconds=.1):
        nonlocal transcript
        transcript += device.read(seconds)
    def terminal():
        return 'SDPUT ERR' in transcript or 'SDPUT OK ' + path in transcript
    def wait(predicate, seconds):
        deadline = time.monotonic() + seconds
        while not predicate() and time.monotonic() < deadline: read()
        if not predicate(): raise TimeoutError('SDPUT handshake/progress timed out')
    def progress():
        values = re.findall(r'SDPUT PART (\d+)', transcript)
        return int(values[-1]) if values else 0
    def write(chunk):
        if device.serial.write(chunk) != len(chunk): raise OSError('Short USB write')
        device.serial.flush()
    try:
        transcript = device.command(f'sdput {len(data)} {zlib.crc32(data):08x} {path}', .1)
        wait(lambda: 'SDPUT READY ' + path in transcript or terminal(), 10)
        if terminal(): raise RuntimeError('SDPUT rejected fixture')
        for offset in range(0, len(data), 128):
            chunk = data[offset:offset+128]; write(chunk)
            target = offset + len(chunk)
            wait(lambda: progress() >= target or terminal(), 10)
            if 'SDPUT ERR' in transcript: raise RuntimeError('SDPUT rejected fixture bytes')
        wait(terminal, 15)
        if 'SDPUT OK ' + path not in transcript: raise RuntimeError('SDPUT fixture CRC failed')
    except Exception as error:
        # A failed host transfer must not feed "home"/"backend set" into the
        # binary receiver. Let in-flight bytes settle, observe cumulative count,
        # then finish only this synthetic frame with padding and await CRC/end.
        if not terminal():
            try:
                read(1.2)
                if re.search(r'SDPUT PART \d+', transcript) and not terminal():
                    received = progress()
                    if not 0 <= received < len(data): raise RuntimeError('Invalid SD progress')
                    while received < len(data) and not terminal():
                        n = min(128, len(data)-received); write(b'\0'*n); received += n
                        wait(lambda: progress() >= received or terminal(), 5)
                    wait(terminal, 5)
            except Exception:
                pass
        raise FixtureTransferError(str(error), terminal()) from error
