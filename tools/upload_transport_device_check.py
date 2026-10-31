"""Check one new Recorder take through a local HTTP fixture, with backend recovery.

Requires an empty live upload queue. Never deletes recordings or existing queue
items. On failure, clears the queue only if its sole item is the new test take.
Media/auth values are discarded by the fixture; only WAV size/hash are retained.
--handoff camera/music/photos/videos verifies cancellation and identical retry.
--missing-receipt returns an empty HTTP 200 once and requires an identical retry.
"""
import argparse,hashlib,json,re,shutil,signal,ssl,subprocess,threading,time,uuid
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
from morning_alarm_device_check import Device
from recording_session_device_check import run_recorder


def run(args):
 expected=threading.Event();fixture={'filename':None,'received':None,'requests':[]};received=threading.Event()
 held=threading.Event();release=threading.Event();handoff=getattr(args,'handoff',None)
 class Handler(BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def reply(self,code,receipt=True):
   # Synthetic fixture receipt only; this is not a cloud-persistence test.
   body=json.dumps({'uploaded':True,'id':str(uuid.UUID(fixture['received']['sha256'][:32])),
                    'kind':'audio'}).encode() if code==200 and receipt else b'{}'
   self.send_response(code);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
  def do_GET(self):self.reply(404)
  def do_POST(self):
   if self.path!='/api/chat':self.close_connection=True;return self.reply(503)
   length=int(self.headers.get('Content-Length','0'))
   if not 0<length<3*1024*1024:self.close_connection=True;return self.reply(413)
   self.connection.settimeout(15);body=self.rfile.read(length)
   message=BytesParser(policy=policy.default).parsebytes(b'Content-Type: '+self.headers.get('Content-Type','').encode()+b'\r\n\r\n'+body)
   parts=list(message.iter_parts());media=next((p for p in parts if p.get_filename()),None)
   if not expected.wait(15) or media is None or media.get_filename()!=fixture['filename']:return self.reply(409)
   data=media.get_payload(decode=True)
   if len(data)<44 or data[:4]!=b'RIFF' or data[8:12]!=b'WAVE' or int.from_bytes(data[4:8],'little')+8!=len(data):return self.reply(422)
   fixture['received']={'wav_bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
      'api_key_present':bool(self.headers.get('X-Api-Key')),'multipart_bytes':len(body)}
   fixture['requests'].append(dict(fixture['received']))
   if getattr(args,'missing_receipt',False) and len(fixture['requests'])==1:
    return self.reply(200,receipt=False)
   if handoff and len(fixture['requests'])==1:
    held.set()
    release.wait(45)
    self.close_connection=True
    return # No success response: the client must abort, then retry this file.
   self.reply(200);received.set()
 server=ThreadingHTTPServer(('0.0.0.0',args.fixture_port),Handler)
 if args.tls_cert:
  context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(args.tls_cert,args.tls_key)
  server.socket=context.wrap_socket(server.socket,server_side=True)
 threading.Thread(target=server.serve_forever,daemon=True).start()
 d=None;original=None;test_path=None;video=None;video_log=None;video_started=None;result={'passed':False,'restored':False}
 def status():
  text=''
  for _ in range(10):
   text+=d.command('backend status',1)
   rows=re.findall(r'\[backend-dev\] url=(\S+) dev=([01])\r?\n',text)
   if rows:return {'url':rows[-1][0],'dev':int(rows[-1][1])}
  raise AssertionError('Missing backend status')
 def queue():
  for _ in range(8):
   text=d.command('queue status',1)
   m=re.search(r'QUEUE STATUS count=(\d+) pending=(\d+) worker=(\d+)',text)
   if m:
    paths=re.findall(r'QUEUE ITEM [^\n]*path=(\S+) error=',text)
    assert len(paths)==int(m[1]),'Incomplete queue observation'
    return int(m[1]),paths
  raise AssertionError('Missing queue status')
 def set_url(url):
  d.command('backend set '+url,2)
  assert status()['url']==url,'Backend update not confirmed'
 def completed_stack():
  # The registry retains the completed worker watermark. Query after each
  # quiescent boundary; concurrent serial printf output may lose an exit line.
  for _ in range(8):
   text=d.command('tasks',.5)
   match=re.search(r'\[tasks\] uploadq\s+configured=(\d+)B minFree=(\d+)B completed=1',text)
   if match:return {'configured':int(match[1]),'min_free':int(match[2])}
  raise AssertionError('Missing completed upload task-registry observation')
 try:
  d=Device(args.port,args.output);d.read(40);d.command('home',1)
  assert queue()[0]==0,'Upload fixture requires an empty queue; nothing changed'
  original=status();p=args.output/'original-backend.json';p.write_text(json.dumps(original));p.chmod(0o600)
  set_url(('https://' if args.tls_cert else 'http://')+args.host+':'+str(args.fixture_port))
  if args.webcam:
   video_log=(args.output/'webcam.log').open('w')
   video=subprocess.Popen([shutil.which('ffmpeg') or 'ffmpeg','-hide_banner','-loglevel','error','-f','avfoundation','-framerate','30','-video_size','1280x720','-pixel_format','uyvy422','-i','C270 HD WEBCAM:none','-an','-c:v','libx264','-pix_fmt','yuv420p','-movflags','+faststart',str(args.output/'device-observation.mp4')],stderr=video_log)
   video_started=time.monotonic()
  text=run_recorder(args.port,args.output,device=d)
  m=re.search(r'\[recorder\] Queued audio upload: (\S+)',text);assert m,'No new take path'
  test_path=m[1];fixture['filename']=Path(test_path).name;expected.set()
  if handoff:
   deadline=time.monotonic()+25
   while not held.is_set() and time.monotonic()<deadline:d.read(.2)
   assert held.is_set(),'Fixture never received the first upload body'
   started=time.monotonic();transition=''
   if handoff=='camera':
    transition=d.command('camera open',.3);marker='[cam] Viewfinder started'
   elif handoff=='videos':
    transition=d.command('videos open',.3)
    marker='[videos] active=1 ready=1'
   else:
    transition=d.command('menu',.4)+d.command('rot -24',.3)
    for _ in range(12):
     scene=d.command('scene',.4)
     selected=re.search(r'id=list.row.(\d+).label[^\n]*text="([^"]*)" fg=0xFFFF bg=0x0000',scene)
     rows=re.findall(r'id=list.row.(\d+).label[^\n]*text="([^"]*)"',scene)
     label='Photos' if handoff=='photos' else 'Music'
     target=next((r for r in rows if r[1].startswith(label)),None)
     if selected and target:
      delta=int(target[0])-int(selected[1])
      if delta:transition+=d.command('rot '+str(delta),.3)
      transition+=d.command('c',.3);break
     transition+=d.command('rot 3',.3)
    else:raise AssertionError('Cannot select handoff destination')
    if handoff=='photos':marker='[photos] render'
    else:
     transition+=d.command('rot -24',.3)+d.command('c',.3);marker='[mp3] START'
   deadline=time.monotonic()+15
   while marker not in transition and time.monotonic()<deadline:
    transition+=d.command('videos status',.2) if handoff=='videos' else d.read(.2)
   assert marker in transition,'Handoff destination did not become ready'
   if handoff not in ('photos','videos'):
    assert '[queue] Worker stopped for voice in ' in transition,'No upload quiescence acknowledgment'
   assert 'Voice pause timed out' not in transition
   state=d.command('queue status',1)
   assert 'count=1 pending=1 worker=0' in state,'Upload still running or item lost during handoff'
   assert 'error=Aborted for voice' in state,'Transfer was not interrupted by resource acquisition'
   assert not received.is_set(),'Fixture replied before interruption'
   result['handoff']={'destination':handoff,'seconds_to_ready':round(time.monotonic()-started,3),'pending_worker_stopped':True}
   result['handoff']['completed_stack']=completed_stack()
   if video_started is not None:result['handoff']['webcam_ready_seconds']=round(time.monotonic()-video_started,3)
   d.read(2) # Leave a short observable interval on the destination screen.
   release.set()
   text=d.command('home',2)
   print('PASS active upload interrupted for '+handoff+'; retained item ready to retry',flush=True)
  else:text=''
  deadline=time.monotonic()+65
  while time.monotonic()<deadline:
   text+=d.read(1)
   if received.is_set() and '[queue] Upload response: 200' in text and queue()[0]==0:break
  else:raise AssertionError('Local fixture upload did not complete')
  assert fixture['received']['api_key_present']
  if handoff:
   assert len(fixture['requests'])==2,'Expected exactly interrupted attempt plus retry'
   assert fixture['requests'][0]==fixture['requests'][1],'Retried WAV or authentication presence changed'
   result['handoff']['identical_retry']=True
  if getattr(args,'missing_receipt',False):
   assert len(fixture['requests'])==2,'Expected unconfirmed HTTP 200 followed by retry'
   assert fixture['requests'][0]==fixture['requests'][1],'Unconfirmed upload retry changed bytes'
   assert 'Upload receipt missing or invalid' in (args.output/'device-check.log').read_text()
   result['missing_receipt']={'retained_and_retried':True,'identical_retry':True}
  stack=completed_stack()
  result.update(passed=True,protocol='https' if args.tls_cert else 'http',fixture=fixture['received'],test_path=test_path,
    completed_stack=stack)
  print('PASS new Recorder WAV, authenticated local '+result['protocol'].upper()+' upload and drained queue',flush=True)
 finally:
  try:
   if d:
    try:
     d.command('home',1)
     if original:
      if test_path is None:
       rows=re.findall(r'\[recorder\] Queued audio upload: (\S+)',(args.output/'device-check.log').read_text())
       if rows:test_path=rows[-1]
      count,paths=queue()
      if count and test_path:
       assert paths==[test_path],'Additional queued items present; refusing broad cleanup'
       d.command('queue clear',2);assert queue()[0]==0
    finally:
     if original:
      set_url(original['url']);assert status()==original;result['restored']=True
      print('Restored original backend; recordings retained',flush=True)
     d.close()
  finally:
   release.set();expected.set();server.shutdown();server.server_close()
   if video:
    if video.poll() is None:video.send_signal(signal.SIGINT)
    assert video.wait(timeout=15) in (0,255),'Webcam capture failed'
   if video_log:video_log.close()
   args.output.mkdir(parents=True,exist_ok=True)
   (args.output/'summary.json').write_text(json.dumps(result,indent=2)+'\n')


if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--host',required=True,help='Host LAN IPv4 address reachable from the device');p.add_argument('--fixture-port',type=int,default=8796);p.add_argument('--webcam',action='store_true')
 p.add_argument('--handoff',choices=['camera','music','photos','videos'],help='Hold first response; acquire foreground resources, then verify identical retry')
 p.add_argument('--missing-receipt',action='store_true',help='Return empty HTTP 200 first; require retained file and identical retry')
 p.add_argument('--tls-cert',type=Path);p.add_argument('--tls-key',type=Path)
 args=p.parse_args();assert bool(args.tls_cert)==bool(args.tls_key),'Provide both TLS certificate and key'
 assert not (args.handoff and args.missing_receipt),'Choose one fault per run'
 run(args)
