"""Measure an existing device video without recording, deleting, or uploading media.

Opens Videos, selects an exact path, plays to completion, optionally checks a
pause, then returns Home. Serial opening can reset the board. Use one serial
owner and a private output directory: boot logs can contain account metadata.
Reported PCM time is delivered audio, not measured speaker/optical A/V sync.
"""
import argparse
import json
import math
import sys
import re
import time
from pathlib import Path

import serial
from camera_quality.run_settle_sweep import Device
from video_av_device_check import read_status


class MeasuredDevice(Device):
    def __init__(self, port, log):
        super().__init__(port, log)
        self.render = {}
        self.heap = []

    def line(self, deadline):
        line = super().line(deadline)
        render = re.search(r'\[video-render\] decoded=(\d+) held=(\d+)\s*$', line)
        if render:
            self.render = dict(zip(('decoded', 'held'), map(int, render.groups())))
        heap = re.search(r'\[health\] Heartbeat uptime=\d+ heap=(\d+)\b', line)
        if heap:
            self.heap.append(int(heap.group(1)))
        if 'Guru Meditation' in line or 'Stack canary watchpoint triggered' in line:
            raise RuntimeError('Device fault; inspect private device.log')
        return line


def wait_status(device, predicate, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = read_status(device, timeout=min(3, max(.1, deadline-time.monotonic())))
        if predicate(state):
            return state
        device.drain(.1)
    raise TimeoutError('Device state did not settle; inspect private device.log')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--clip', required=True, help='Existing /videos/NAME.avi')
    parser.add_argument('--timeout', type=float, default=360, help='Playback wall-time limit')
    parser.add_argument('--expected-audio-ms', type=int)
    parser.add_argument('--pause-at', type=float, help='Pause once after this delivered-audio time in seconds')
    args = parser.parse_args()
    if not re.fullmatch(r'/videos/[^/\r\n]+\.avi', args.clip) or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('An exact video path and positive timeout are required')
    if args.pause_at is not None and (not math.isfinite(args.pause_at) or args.pause_at <= 0):
        parser.error('--pause-at must be positive')
    if args.expected_audio_ms is not None and args.expected_audio_ms < 0:
        parser.error('--expected-audio-ms must be nonnegative')
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    connection = serial.Serial(port=None, baudrate=115200, timeout=.1, exclusive=True)
    connection.dtr = connection.rts = False
    connection.port = args.port
    result = {'clip': args.clip, 'completed': False, 'passed': False, 'samples': []}
    device = None
    try:
        connection.open()
        with (args.output/'device.log').open('w') as log:
            device = MeasuredDevice(connection, log)
            try:
                device.drain(35)
                device.send('home'); device.drain(2)
                device.send('videos open'); device.drain(3)
                # Preview admission is asynchronous: a zero catalog count
                # while background work drains is not evidence of missing media.
                state = wait_status(device, lambda s: int(s['count'])>0 or s['error']=='1', 30)
                for _ in range(int(state['count'])):
                    if state['path'] == args.clip:
                        break
                    device.send('rot 1'); device.drain(.5)
                    state = read_status(device)
                if state['path'] != args.clip:
                    raise RuntimeError('Requested clip not in device catalog')
                state = wait_status(device, lambda s: s['ready']=='1' or s['error']=='1', 30)
                if state['error']=='1':
                    raise RuntimeError('Selected clip failed to load')
                device.heap.clear()
                started = time.monotonic()
                device.send('b')
                paused = False
                while time.monotonic()-started < args.timeout:
                    state = read_status(device)
                    if state['error']=='1' or state['active']!='1':
                        raise RuntimeError('Playback failed or left Videos')
                    result['samples'].append(dict(state, wall_seconds=time.monotonic()-started,
                                                  **device.render))
                    if state['ended']=='1':
                        result['completed'] = True
                        break
                    if args.pause_at is not None and not paused and int(state['audio_ms'])>=args.pause_at*1000:
                        device.send('b')
                        before = wait_status(device, lambda s: s['playing']=='0', 5)
                        device.drain(2)
                        after = read_status(device)
                        if (before['audio_ms'], before['frame']) != (after['audio_ms'], after['frame']):
                            raise RuntimeError('Media advanced while paused')
                        device.send('b')
                        wait_status(device, lambda s: s['playing']=='1', 5)
                        paused = True
                    device.drain(5)
                if not result['completed']:
                    raise TimeoutError('Playback did not finish within the wall-time limit')
                if args.pause_at is not None and not paused:
                    raise RuntimeError('Clip ended before the requested pause check')
                if args.expected_audio_ms is not None and int(state['audio_ms']) != args.expected_audio_ms:
                    raise RuntimeError('Delivered audio duration differs from the expected duration')
                result['pause_checked'] = paused
                result['heap_samples'] = list(device.heap)
                print(json.dumps(result['samples'][-1]), flush=True)
            finally:
                result['heap_samples'] = list(device.heap)
                result['pause_checked'] = paused if 'paused' in locals() else False
                # Preserve the original failure if cleanup also loses the device.
                failing = sys.exc_info()[0] is not None
                try:
                    device.send('home'); device.drain(2)
                    state = read_status(device)
                    result['returned_home'] = state['active']=='0' and state['playing']=='0'
                    device.send('tasks'); device.drain(2)
                    if not result['returned_home']:
                        raise RuntimeError('Videos did not acknowledge exit')
                except Exception as error:
                    result['cleanup_error'] = str(error)
                    if not failing:
                        raise
            result['passed'] = True
    except KeyboardInterrupt:
        result['error'] = 'Interrupted by operator'
        raise
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        connection.close()
        (args.output/'result.json').write_text(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
