"""Exercise the production lesson state and native rendering repeatedly."""
from pathlib import Path
import json, os, shlex, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(dir=ROOT/'.cache') as temp:
 exe=Path(temp)/'controls.exe'
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-I'+str(ROOT/'include'),str(ROOT/'test/onboarding_host/controls.cpp'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS 1000 repeated native hold/release, wrong button, reversal, threshold, rollback and timer-wrap runs')
binary=ROOT/'.cache/mp3-preview/render.exe'
def render(page,events,now):
 return json.loads(subprocess.check_output([str(binary),'--control-demo',str(page),events,str(now)]))
def pixel(frame,x,y):
 color=None
 for xx,yy,w,h,c in frame['pixels']:
  if xx<=x<xx+w and yy<=y<yy+h:color=c
 return color
for direction in [1]:
 events='0,d,0;100,u,0;700,w,0;710,d,1;800,u,1;1400,w,0;'
 events+=';'.join(f'{1410+i*10},r,{direction}' for i in range(8))
 for repeat in range(20):
  f=render(3,events,1480);assert f['page']==6 and f['success'] and f['angle']==180 and f['haptics']==10
  assert pixel(f,30,120)==0x364a 
  held=events+';2100,w,0;2200,d,2;10000,w,0'
  f=render(3,held,10000);assert f['page']==5 and f['held'] and not f['success'] and f['haptics']==10
  assert not f["success"]
  released=held+';10010,u,2'
  f=render(3,released,10010);assert f['success'] and not f['held'] and f['haptics']==11
  assert pixel(f,30,120)==0x364a
  f=render(3,released+';10560,w,0',10560);assert f['done'] and f['page']==36 and f['haptics']==11
print('PASS 20 complete native rendered sequences, hard stops, held C, green success, haptic counts and final selection')
