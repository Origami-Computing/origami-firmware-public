"""Combine an explicitly identified legacy AVI/WAV pair for on-device playback.
Preserves original JPEG/PCM bytes and both input files. Does not guess pairing or
an unknown capture offset; video/audio begin at zero. No device or network access.
"""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--video',type=Path,required=True);parser.add_argument('--audio',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
 args=parser.parse_args()
 if args.output.exists():raise SystemExit('Output already exists')
 metadata=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','a','-show_streams','-of','json',str(args.audio)]))['streams']
 if len(metadata)!=1 or metadata[0]['codec_name']!='pcm_s16le' or metadata[0]['sample_rate']!='48000' or metadata[0]['channels']!=1:
  raise SystemExit('Requires original 48 kHz mono PCM16 WAV')
 with tempfile.TemporaryDirectory() as directory:
  p=Path(directory)
  subprocess.run(['ffmpeg','-v','error','-i',str(args.audio),'-map','0:a:0','-c:a','copy','-f','s16le',str(p/'audio.pcm')],check=True)
  subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'include'),str(ROOT/'tools/mux_device_recording.cpp'),'-o',str(p/'mux')],check=True)
  subprocess.run([str(p/'mux'),str(args.video.resolve()),str(p/'audio.pcm'),str(args.output.resolve())],check=True)
  subprocess.run(['ffmpeg','-v','error','-i',str(args.output),'-map','0:a:0','-f','s16le',str(p/'verified.pcm')],check=True)
  if (p/'audio.pcm').read_bytes()!=(p/'verified.pcm').read_bytes():raise RuntimeError('Combined audio verification failed')
  subprocess.run(['ffmpeg','-v','error','-i',str(args.output),'-map','0:v:0','-f','null','-'],check=True)
  def jpeg_hashes(path):
   return json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0','-show_packets','-show_data_hash','sha256','-show_entries','packet=data_hash','-of','json',str(path)]))['packets']
  if jpeg_hashes(args.video)!=jpeg_hashes(args.output):raise RuntimeError('Original JPEG preservation check failed')
 print(f'Validated {args.output.resolve()}')
if __name__=='__main__':main()
