"""Exercise actual recording Stop/Back/C/Home and re-entry on a connected board.

Requires development firmware and pyserial. Creates four short recordings on SD,
keeps all media and configuration, uploads nothing, and ends at Home. Checks
serial ownership/finalization evidence; not full-file, optical or AV-sync proof.
"""
from camera_video_controls import select_video_with_audio
import argparse
import json
from pathlib import Path
import re
import time
import serial
from camera_quality.run_settle_sweep import Device


class CheckedDevice(Device):
    def __init__(self, port, log):
        super().__init__(port, log)
        self.events = []

    def line(self, deadline):
        line = super().line(deadline)
        self.events.append(line)
        if any(marker in line for marker in ('Guru Meditation', 'assert failed', '[video] failed', "FAILED to spawn 'video")):
            raise AssertionError(line)
        return line


def run(port, output):
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    connection = serial.Serial(port=None, baudrate=115200, timeout=.1, exclusive=True)
    connection.dtr = connection.rts = False
    connection.port = port
    connection.open()
    results = []
    with (output / 'device.log').open('w') as log:
        device = CheckedDevice(connection, log)
        def encoder():
            device.send('cp'); device.drain(.35); device.send('cr')
        try:
            device.drain(45)
            device.send('home'); device.drain(2)
            for action in ('back', 'home', 'encoder', 'stop'):
                start = len(device.events)
                device.send('camera open')
                device.wait_for('[cam] Viewfinder started', 25)
                device.drain(.5)
                select_video_with_audio(device)
                device.send('b')
                device.wait_for("[tasks] spawned 'video_capture'", 20)
                device.drain(2)
                requested = time.monotonic()
                if action == 'encoder': encoder()
                else: device.send({'back': 'a', 'home': 'home', 'stop': 'b'}[action])
                device.wait_for('[video] saved ', 20)
                saved_ms = round((time.monotonic() - requested) * 1000)
                device.wait_for('[video-audio] ', 5)
                if action == 'home':
                    device.wait_for('[cam] Camera stopped', 15)
                else:
                    device.drain(1.5)
                    if action == 'encoder':
                        # A second C opens settings only after recording stops;
                        # A returns to preview without changing any setting.
                        encoder(); device.wait_for('[cam] Camera stopped', 15)
                        device.drain(.5); device.send('a')
                        device.wait_for('[cam] Viewfinder started', 25)
                    device.send('home'); device.wait_for('[cam] Camera stopped', 15)
                device.drain(1)
                text = '\n'.join(device.events[start:])
                clips = re.findall(r'\[video\] saved path=(\S+) frames=(\d+) dropped=(\d+)', text)
                samples = re.findall(r'\[video-audio\] samples=(\d+)', text)
                assert len(clips) == 1 and len(samples) == 1, 'Expected exactly one finalized recording'
                assert int(clips[0][1]) > 0 and int(samples[0]) >= 48000
                assert text.index('[video] saved ') < text.index('[cam] Camera stopped'), 'Camera released before finalization report'
                assert clips[0][0] not in [row['path'] for row in results]
                row = {'action': action, 'path': clips[0][0], 'frames': int(clips[0][1]),
                       'drops': int(clips[0][2]), 'audio_samples': int(samples[0]),
                       'request_to_saved_ms': saved_ms, 'returned_home': True}
                results.append(row)
                (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps(row), flush=True)
            device.send('tasks'); device.drain(2)
        finally:
            try: device.send('home'); device.drain(1)
            finally: connection.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    run(args.port, args.output)
