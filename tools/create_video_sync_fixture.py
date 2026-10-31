"""Create a four-second synthetic VGA/PCM AVI: flashes and tones at 1, 2, 3 s.
Requires C++17 and FFmpeg. Writes only the explicit output file; never contacts a device.
"""
from pathlib import Path
import argparse,math,struct,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
 if args.output.exists():raise SystemExit('Output already exists')
 with tempfile.TemporaryDirectory() as directory:
  p=Path(directory)
  for name,color in [('dark','0x303030'),('light','white')]:
   filters=f"drawbox=x=220:y=140:w=200:h=200:color={color}:t=fill"
   subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=c=0x202020:s=640x480','-vf',filters,'-frames:v','1','-q:v','5',str(p/(name+'.jpg'))],check=True)
  pcm=bytearray()
  for n in range(192000):
   second,within=divmod(n,48000);active=1<=second<=3 and within<9600
   envelope=min(1,within/240,(9599-within)/240) if active else 0
   value=round(4000*envelope*math.sin(2*math.pi*700*n/48000)) if active else 0
   pcm.extend(struct.pack('<h',value))
  (p/'audio.pcm').write_bytes(pcm)
  subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'include'),str(ROOT/'tools/video_sync_fixture.cpp'),'-o',str(p/'pack')],check=True)
  subprocess.run([str(p/'pack'),str(p/'dark.jpg'),str(p/'light.jpg'),str(p/'audio.pcm'),str(args.output.resolve())],check=True)
 print(f'Created {args.output.resolve()} ({args.output.stat().st_size} bytes)')
if __name__=='__main__':main()
