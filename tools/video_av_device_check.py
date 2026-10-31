"""Install an isolated synthetic AV fixture and test native preview/play/pause/replay.
Requires a fixture from create_video_sync_fixture.py. Creates one test-*.avi on
SD and preserves it for listening/visual checks; does not change settings or upload.
"""
import argparse,json,re,time
from pathlib import Path
import serial
from camera_quality.run_settle_sweep import Device
from upload_fixture_serial import put_fixture,FixtureTransferError

STATUS_FIELDS='active ready playing ended error index count frame total audio audio_ms skipped elapsed_ms decode_max_ms present_max_ms'.split()
STATUS_PATTERN=re.compile(r'\[videos\] '+''.join(rf'{name}=(?P<{name}>\d+) ' for name in STATUS_FIELDS)+r'path=(?P<path>\S*)')

def parse_status(line):
 # Ordinary firmware logs share USB and can prefix a complete record. Require
 # every ordered field: a truncated/interleaved record is not valid evidence.
 match=STATUS_PATTERN.search(line)
 if not match:return None
 status=match.groupdict()
 if int(status['count']) and not (status['path'].startswith('/videos/') and status['path'].endswith('.avi')):return None
 return status

def read_status(device,timeout=10):
 end=time.monotonic()+timeout
 message=''
 for attempt in range(3):
  device.send('videos status')
  deadline=min(end,time.monotonic()+timeout/3)
  while time.monotonic()<deadline:
   try:line=device.line(deadline)
   except TimeoutError:break
   if 'Guru Meditation' in line:raise RuntimeError(line)
   if '[videos-error] ' in line:message=line.split('[videos-error] ',1)[1]
   status=parse_status(line)
   if status is not None:
    status['message']=message if status['error']=='1' else ''
    return status
  if time.monotonic()>=end:break
 raise TimeoutError('No complete video status response; see device.log')

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--port',required=True);parser.add_argument('--output',type=Path,required=True)
 source=parser.add_mutually_exclusive_group(required=True)
 source.add_argument('--fixture',type=Path)
 source.add_argument('--existing',help='Previously CRC-verified /videos/test-av-sync-N.avi; avoids another upload')
 args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
 remote=args.existing or f'/videos/test-av-sync-{int(time.time())}.avi'
 if not re.fullmatch(r'/videos/test-av-sync-[0-9]+\.avi',remote):raise ValueError('Requires an isolated synthetic sync fixture')
 connection=serial.Serial(port=None,baudrate=115200,timeout=.1,write_timeout=5,exclusive=True)
 connection.dtr=False;connection.rts=False;connection.port=args.port;connection.open()
 serial_ready=True
 try:
  with (args.output/'device.log').open('w') as log:
   d=Device(connection,log)
   class FixtureDevice:
    serial=connection
    def read(self,seconds):
     try:return d.line(time.monotonic()+seconds)+'\n'
     except TimeoutError:return ''
    def command(self,command,seconds):d.send(command);return self.read(seconds)
   def status():
    return read_status(d)
   def until(predicate,timeout=15):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
     s=status();assert s['error']=='0',s
     if predicate(s):return s
     d.drain(.2)
    raise AssertionError('Playback state timeout')
   d.drain(20);d.send('home');d.drain(2);d.send('menu');d.drain(2)
   if args.fixture:
    try:put_fixture(FixtureDevice(),remote,args.fixture.read_bytes())
    except FixtureTransferError as error:serial_ready=error.serial_ready;raise
    print('Fixture transferred with device CRC verification',flush=True)
   d.send('videos open');d.drain(2);s=status()
   # Preview now retains admission while background work drains. Do not mistake
   # the pending empty catalog for missing media or inject a second request.
   admission_deadline=time.monotonic()+30
   while s['count']=='0' and s['error']=='0' and time.monotonic()<admission_deadline:
    d.drain(.2);s=status()
   assert int(s['count'])>0,'Preview admission failed or catalog is empty'
   for _ in range(int(s['count'])):
    if s['path']==remote:break
    d.send('rot 1');d.drain(.5);s=status()
   s=until(lambda s:s['path']==remote and s['ready']=='1')
   assert s['path']==remote and s['audio']=='1' and s['frame']=='1' and s['playing']=='0',s
   results={'path':remote,'admission_retries':0,'preview':s}
   d.send('b');d.drain(.6);results['playing']=until(lambda s:s['playing']=='1' and int(s['audio_ms'])>0)
   d.send('b');d.drain(.4);paused=status();d.drain(.8);paused2=status()
   assert paused2['playing']=='0' and paused['audio_ms']==paused2['audio_ms'] and paused['frame']==paused2['frame'],(paused,paused2)
   results['paused']=paused2;d.send('b');d.drain(.2)
   results['ended']=until(lambda s:s['ended']=='1')
   assert results['ended']['audio_ms']=='4000' and results['ended']['frame']=='100',results['ended']
   d.send('b');d.drain(.3);results['replay']=until(lambda s:s['playing']=='1' and int(s['audio_ms'])<3000)
   d.send('home');d.drain(2);s=status();assert s['active']=='0' and s['ready']=='0',s
   d.send('tasks');d.drain(1)
   (args.output/'result.json').write_text(json.dumps(results,indent=2));print(json.dumps(results,indent=2))
 finally:
  # Never send commands into an unfinished binary upload.
  if serial_ready:
   try:connection.write(b'home\n');connection.flush()
   except serial.SerialException:pass
  connection.close()
if __name__=='__main__':main()
