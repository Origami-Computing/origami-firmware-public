"""Exercise first-frame preview, play/pause/replay, clip selection and exit on device.
No recording, deletion, upload or setting changes. Requires existing AVI clips.
"""
import argparse
import json
from pathlib import Path
import re
import serial
import time
from camera_quality.run_settle_sweep import Device


def run(port, output):
    output.mkdir(parents=True,exist_ok=True)
    connection=serial.Serial()
    connection.port=port;connection.baudrate=115200;connection.timeout=.1
    connection.dtr=connection.rts=False;connection.exclusive=True;connection.open()
    with (output/'playback.log').open('w') as log:
        d=Device(connection,log)
        def command(value,delay=.5):
            d.send(value);d.drain(delay)
        def status():
            d.send('videos status')
            line=d.wait_for('[videos] ',10)[-1]
            return dict(re.findall(r'(\w+)=(\S+)',line))
        def until(predicate,timeout=12):
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                s=status()
                assert s['error']=='0',s
                if predicate(s):return s
                d.drain(.2)
            raise AssertionError('Playback state timeout: '+str(s))
        results={}
        try:
            # Native USB may reset the board on a new host connection. Wait for
            # boot composition/navigation, not just the early serial command loop.
            d.drain(20)
            command('home',2);command('videos open',1)
            initial=until(lambda s:s['ready']=='1')
            assert initial['frame']=='1' and initial['playing']=='0',initial
            assert int(initial['total'])>=5, 'Use a clip long enough to test pause'
            results['preview']=initial
            command('b',.6)
            playing=until(lambda s:int(s['frame'])>1 and s['playing']=='1')
            command('b',.3);paused=status();d.drain(.8);paused2=status()
            assert paused['playing']=='0' and paused['frame']==paused2['frame'],(paused,paused2)
            results['paused']=paused2
            command('b',.2)
            ended=until(lambda s:s['ended']=='1',30)
            assert ended['frame']==ended['total'] and ended['playing']=='0',ended
            results['ended']=ended
            command('b',.3)
            replay=until(lambda s:s['ready']=='1' and s['playing']=='1' and int(s['frame'])<int(s['total']))
            results['replay']=replay
            if int(initial['count'])>1:
                command('rot 1',.5)
                selected=until(lambda s:s['index']=='2' and s['ready']=='1')
                assert selected['frame']=='1' and selected['playing']=='0',selected
                results['selection']=selected
            command('home',2)
            inactive=status();assert inactive['active']=='0' and inactive['ready']=='0',inactive
            d.send('tasks');lines=[];deadline=time.monotonic()+2
            try:
                while True:lines.append(d.line(deadline))
            except TimeoutError:pass
            watermarks=[line for line in lines if 'video_read' in line or 'video_write' in line]
            assert any('video_read' in line and 'completed=1' in line for line in watermarks),watermarks
            results['watermarks']=watermarks
            (output/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
            print(json.dumps(results,indent=2))
        finally:
            try:command('home',1)
            except serial.SerialException:pass
            finally:connection.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();run(args.port,args.output)
