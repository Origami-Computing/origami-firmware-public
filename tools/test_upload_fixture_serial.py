"""Exercise real fixture framing with delayed handshakes and interrupted USB writes."""
import types,zlib
import upload_fixture_serial as target
class Device:
 def __init__(self,ready_after=.8,short=False,bad_crc=False,silent=False):
  self.now=0.;self.ready_after=ready_after;self.short=short;self.bad_crc=bad_crc;self.silent=silent
  self.serial=self;self.commands=[];self.data=bytearray();self.sent_ready=False;self.done=False;self.writes=0
 def command(self,text,wait):
  self.commands.append(text);_,n,crc,self.path=text.split();self.expected=int(n);self.crc=int(crc,16)
  return self.read(wait)
 def read(self,seconds):
  self.now+=seconds
  if self.silent or self.now<self.ready_after:return 'background log\n'
  output=''
  if not self.sent_ready:self.sent_ready=True;output+='SDPUT READY '+self.path+'\n'
  if not self.done:
   if len(self.data)==self.expected:
    self.done=True;output+=('SDPUT ERR crc\n' if self.bad_crc or zlib.crc32(self.data)!=self.crc else 'SDPUT OK '+self.path+'\n')
   else:output+='SDPUT PART '+str(len(self.data))+'\n'
  return output
 def write(self,chunk):
  self.writes+=1
  assert self.sent_ready,'Host wrote before ready'
  if self.short and self.writes==2:return 0
  assert len(chunk)<=128
  self.data+=chunk;assert len(self.data)<=self.expected
  return len(chunk)
 def flush(self):pass
fixture=b'jpeg'+bytes(range(256))*2
for delay in [.8,2.,9.]:
 d=Device(ready_after=delay);target.time=types.SimpleNamespace(monotonic=lambda:d.now)
 target.put_fixture(d,'/upload_queue/test-ready.jpg',fixture)
 assert d.data==fixture and d.done and len(d.commands)==1
for options,ready in [({'short':True},True),({'bad_crc':True},True),({'silent':True},False),({'ready_after':10.8},True)]:
 d=Device(**options);target.time=types.SimpleNamespace(monotonic=lambda:d.now)
 try:target.put_fixture(d,'/upload_queue/test-fault.jpg',fixture)
 except target.FixtureTransferError as error:assert error.serial_ready==ready
 else:raise AssertionError('Fault unexpectedly passed')
 assert len(d.commands)==1
 if ready:assert d.done and len(d.data)==len(fixture)
 else:assert not d.data
print('PASS fixture serial: delayed READY, bounded chunks, short-write/CRC recovery, no commands inside binary frame')

for path in ['/videos/test-av-sync.avi','/upload_queue/test-video.avi']:
 d=Device();target.time=types.SimpleNamespace(monotonic=lambda:d.now)
 target.put_fixture(d,path,fixture);assert d.data==fixture and d.done
for path in ['/videos/VID_user.avi','/videos/../test-user.avi','/audio/user.wav']:
 try:target.put_fixture(Device(),path,fixture)
 except ValueError:pass
 else:raise AssertionError('Non-fixture destination accepted')
