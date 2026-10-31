"""Record two short silent clips through Camera UI; retrieve one with CRC verification.
Requires the video firmware, pyserial and ffmpeg/ffprobe. Saves private logs and
camera media only to --output; leaves the recorded clips on SD. Does not change
persisted Camera settings. Ends at home. No flashing or backend configuration.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
import serial
from camera_quality.run_settle_sweep import Device


def run(port, output):
    output.mkdir(parents=True, exist_ok=True)
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.timeout = .1
    connection.dtr = connection.rts = False
    connection.exclusive = True
    connection.open()
    with (output/'device.log').open('w') as log:
        d = Device(connection, log)
        def command(text, delay=.5):
            if text == 'c':
                d.send('cp'); d.drain(.4); d.send('cr'); d.drain(max(.4, delay))
            else:
                d.send(text); d.drain(delay)
        def scene():
            d.send('scene')
            lines=[]
            deadline=time.monotonic()+1
            try:
                while True: lines.append(d.line(deadline))
            except TimeoutError:
                return lines
        def open_video():
            command('home', 2)
            d.send('camera open'); d.wait_for('[cam] Viewfinder started')
            command('c', 2)
            has_autofocus = any('Autofocus  ' in line for line in scene())
            command('rot -30');command('rot '+str(3 if has_autofocus else 2));command('c');command('rot 1');command('c')
            lines=scene()
            assert any('Video silent' in line for line in lines), 'Video setting missing'
            d.send('a');d.wait_for('[cam] Viewfinder started')
            d.drain(1)
        results=[]
        try:
            d.drain(3)
            for stop in ['b','a']:
                open_video()
                command('b', 4)
                d.send(stop)
                line=d.wait_for('[video] ', 20)[-1]
                match=re.fullmatch(r'\[video\] saved path=(\S+) frames=(\d+) dropped=(\d+)',line)
                assert match, line
                assert int(match[2]) >= 5, line
                results.append({'path':match[1],'frames':int(match[2]),'dropped':int(match[3]),'stop':stop})
                command('home',2)
                destination=output/f'clip-{stop}.avi'
                d.download(match[1],destination)
                metadata=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(destination)]))
                stream=metadata['streams'][0]
                assert stream['codec_name']=='mjpeg' and len(metadata['streams'])==1
                assert int(stream['nb_frames'])==int(match[2])
                subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(destination),'-f','null','-'],check=True)
                results[-1].update({key:stream[key] for key in ['width','height','duration','avg_frame_rate']})
                (output/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
                print(json.dumps(results[-1]), flush=True)
            # Re-entry returns to Photo, retaining the persisted still settings.
            d.send('camera open');d.wait_for('[cam] Viewfinder started');command('c',2)
            lines=scene()
            assert any('Recording  Photo' in line for line in lines)
            command('home',2)
            d.send('tasks')
            d.drain(1)
            (output/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
        finally:
            try:
                command('home',1)
            except serial.SerialException:
                pass  # Preserve the original disconnect/error; still close the port.
            finally:
                connection.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    run(args.port,args.output)
