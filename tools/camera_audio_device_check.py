"""Record combined camera/mic AVI, retrieve with CRC, and test native playback.
Requires development firmware, pyserial, ffmpeg and ffprobe. Leaves the new
recording on SD, preserves settings/media, uploads nothing, and ends at Home.
Use --skip-download to leave retrieval/local decode for a later web transfer.
Serial timing is not physical speaker/lipsync acceptance.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
import serial
from camera_quality.run_settle_sweep import Device
from video_av_device_check import read_status
from camera_video_controls import select_video_with_audio
from video_playback_benchmark import wait_status


def capture_metrics(log_path):
    # Copy only numeric diagnostics, never unrelated serial/configuration traffic.
    diagnostics={}
    with log_path.open() as stream:
        for line in stream:
            for tag in ('video-perf','video-submit','video-write'):
                marker='['+tag+'] '
                if marker in line:
                    diagnostics[tag]=dict((key,int(value)) for key,value in
                        re.findall(r'([a-z_]+)=(\d+)',line.split(marker,1)[1]))
    return diagnostics


def run(port, output, seconds, skip_download=False, brightness=None):
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    connection = serial.Serial(port=None, baudrate=115200, timeout=.1, exclusive=True)
    connection.dtr = connection.rts = False
    connection.port = port
    connection.open()
    with (output/'device.log').open('w') as log:
        d = Device(connection, log)
        original_brightness = None
        try:
            d.drain(45)
            d.send('brightness')
            status = d.wait_for('[brightness] ', 5)[-1]
            match = re.search(r'ok=1 percent=(\d+) saved=(\d+)', status)
            assert match, 'Brightness status unavailable'
            original_brightness = int(match[1])
            if brightness is not None:
                d.send(f'brightness {brightness}')
                status = d.wait_for('[brightness] ', 5)[-1]
                assert f'ok=1 percent={brightness} ' in status, status
            d.send('home'); d.drain(2)
            d.send('camera open'); d.wait_for('[cam] Viewfinder started', 25)
            d.drain(1)
            select_video_with_audio(d)
            d.send('b')  # Video + audio selected through ordinary Settings.
            started = time.monotonic()
            deadline = started + seconds + 30
            video = audio = None
            stop_sent = False
            next_progress = started + 30
            # Read throughout capture: the firmware can auto-stop before our
            # timer (duration/size/storage limits). Never lose its save event or
            # press B again after auto-stop and accidentally start another clip.
            while video is None or audio is None:
                now = time.monotonic()
                if now >= deadline:
                    raise TimeoutError('Recording did not finalize; inspect device.log')
                if now >= next_progress:
                    print(f'Capture/finalization elapsed {int(now-started)} s', flush=True)
                    next_progress = now + 30
                if seconds < 300 and now >= started + seconds and not stop_sent and video is None:
                    d.send('b'); stop_sent = True
                try:
                    line = d.line(min(deadline, now + .5))
                except TimeoutError:
                    continue
                if 'Guru Meditation' in line or '[video] failed' in line:
                    raise AssertionError(line)
                match = re.search(r'\[video\] saved path=(\S+) frames=(\d+) dropped=(\d+)', line)
                if match: video = dict(zip(['path','frames','dropped'], match.groups()))
                match = re.search(r'\[video-audio\] samples=(\d+) repeated=(\d+)', line)
                if match: audio = dict(zip(['samples','repeated'], match.groups()))
            (output/'capture.json').write_text(json.dumps({'video':video,'audio':audio},indent=2)+'\n')
            assert int(audio['samples']) > 0, audio
            d.send('home'); d.drain(2)
            d.send('menu'); d.drain(2)
            streams = []
            assert abs(int(audio['samples'])/48000-seconds)<2, audio
            if not skip_download:
                # USB retrieval emits one bounded packet per UI tick. Large AVI
                # files take minutes; never replace CRC checking with a partial copy.
                for attempt in range(8):
                    try:
                        d.download(video['path'], output/'video.avi', timeout=1800)
                        break
                    except RuntimeError as error:
                        if 'background_busy' not in str(error) or attempt==7: raise
                        d.drain(10)
                metadata = json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(output/'video.avi')]))
                streams = metadata['streams']
                assert len(streams)==2 and streams[0]['codec_name']=='mjpeg' and streams[1]['codec_name']=='pcm_s16le', streams
                assert streams[1]['sample_rate']=='48000' and streams[1]['channels']==1, streams
                subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(output/'video.avi'),'-f','null','-'],check=True)
            d.send('videos open'); d.drain(2)
            status = wait_status(d, lambda state: int(state['count'])>0 or state['error']=='1', 30)
            for _ in range(int(status['count'])):
                if status['path']==video['path']: break
                d.send('rot 1'); d.drain(.5); status=read_status(d)
            status = wait_status(d, lambda state: state['ready']=='1' or state['error']=='1', 30)
            assert status['path']==video['path'] and status['audio']=='1' and status['ready']=='1', status
            d.send('b')
            deadline = time.monotonic()+seconds+25
            while time.monotonic()<deadline:
                status=read_status(d)
                assert status['error']=='0',status
                if status['ended']=='1':break
                d.drain(.3)
            assert status['ended']=='1',status
            assert abs(int(status['audio_ms'])-int(audio['samples'])*1000/48000)<50,status
            d.send('home'); d.drain(2); d.send('tasks'); d.drain(1)
            log.flush()
            diagnostics=capture_metrics(output/'device.log')
            result={'video':video,'audio':audio,'streams':streams,'playback':status,
                    'capture_metrics':diagnostics,
                    'downloaded_and_decoded':not skip_download,
                    'brightness':brightness if brightness is not None else original_brightness,
                    'limits':'Serial state only when download is skipped; physical sound and AV offset require listening/measurement.'}
            (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
            print(json.dumps(result,indent=2),flush=True)
        finally:
            try:
                if brightness is not None and original_brightness is not None:
                    d.send(f'brightness {original_brightness}')
                    restored=d.wait_for('[brightness] ', 5)[-1]
                    assert f'ok=1 percent={original_brightness} ' in restored, 'Brightness restoration failed'
                d.send('home'); d.drain(1)
            finally: connection.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--seconds',type=int,default=8)
    parser.add_argument('--skip-download',action='store_true',help='Keep original on SD; skip USB retrieval and local codec validation')
    parser.add_argument('--brightness', type=int, help='Temporary output brightness; original live value restored without NVS writes')
    args=parser.parse_args()
    if not 3<=args.seconds<=300:parser.error('--seconds must be 3..300')
    if args.brightness is not None and not 5<=args.brightness<=100:parser.error('--brightness must be 5..100')
    run(args.port,args.output,args.seconds,args.skip_download,args.brightness)
