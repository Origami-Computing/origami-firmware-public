"""Validate production MJPEG/PCM AVI codec with independent FFmpeg demux/decode."""
from pathlib import Path
import json,shutil,subprocess,tempfile
from audit_video_frames import audit
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);jpeg=p/'frame.jpg'
 if shutil.which('ffmpeg'):
  subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=c=blue:s=640x480','-frames:v','1',str(jpeg)],check=True)
 else:jpeg.write_bytes(b'\xff\xd8\x42\xff\xd9')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(ROOT/'tests/video/container.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(jpeg),str(p/'clip.avi'),str(p/'expected.pcm')],check=True)
 if shutil.which('ffmpeg') and shutil.which('ffprobe'):
  assert audit((p/'clip.avi').read_bytes())['defective_chunks']==0
  result=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(p/'clip.avi')]))
  video,audio=result['streams'];assert video['codec_name']=='mjpeg' and video['nb_frames']=='8'
  assert abs(float(video['duration'])-.8)<.001
  assert audio['codec_name']=='pcm_s16le' and audio['sample_rate']=='48000' and audio['channels']==1
  subprocess.run(['ffmpeg','-v','error','-i',str(p/'clip.avi'),'-map','0:v','-f','null','-'],check=True)
  subprocess.run(['ffmpeg','-v','error','-i',str(p/'clip.avi'),'-map','0:a','-f','s16le',str(p/'decoded.pcm')],check=True)
  assert (p/'expected.pcm').read_bytes()==(p/'decoded.pcm').read_bytes()
  print('PASS FFmpeg: two streams, eight timeline intervals including holds and 38,400 bit-exact PCM samples')
  # Reproduce the device defect using real decodable JPEGs and the production
  # container writer. Codec success alone must not admit joined images.
  joined=p/'joined.jpg';joined.write_bytes(jpeg.read_bytes()+b'\0'*13+jpeg.read_bytes())
  subprocess.run([str(p/'test'),str(joined),str(p/'joined.avi'),str(p/'joined.pcm')],check=True)
  report=audit((p/'joined.avi').read_bytes())
  assert report['jpeg_chunks']==5 and report['defective_chunks']==5
  decode=subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(p/'joined.avi'),'-map','0:v','-f','null','-'],capture_output=True)
  print(f'PASS structural audit rejects all joined-image chunks (FFmpeg exit={decode.returncode})')
  # Real 720p JPEGs through the same bounded writer/parser and independent decoder.
  hd=p/'hd.jpg'
  subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=s=1280x720','-frames:v','1',str(hd)],check=True)
  subprocess.run([str(p/'test'),str(hd),str(p/'hd.avi'),str(p/'hd.pcm'),'1280','720'],check=True)
  metadata=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(p/'hd.avi')]))
  assert (metadata['streams'][0]['width'],metadata['streams'][0]['height'])==(1280,720)
  subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(p/'hd.avi'),'-f','null','-'],check=True)
  subprocess.run(['ffmpeg','-v','error','-i',str(p/'hd.avi'),'-map','0:a','-f','s16le',str(p/'hd-decoded.pcm')],check=True)
  assert (p/'hd.pcm').read_bytes()==(p/'hd-decoded.pcm').read_bytes()
  print('PASS HD container: 1280x720 independent decoding and exact PCM')
 else:print('SKIP external decoding: FFmpeg/ffprobe unavailable')
