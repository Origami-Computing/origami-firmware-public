"""Exercise production asynchronous playback scheduling with delayed codec results."""
from pathlib import Path
import runpy, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
base=runpy.run_path(str(ROOT/'tools/test_video_playback_timing.py'))
function=base['function']
code=base['code'].split('static uint32_t playbackTimeMs(')[0]
code=code.replace('bool copyCompositeForDevelopment', 'bool prepareDecoder(bool){return !fail;} bool copyDecoded(const uint16_t*,bool){++decodes;return !fail;}\n bool copyCompositeForDevelopment')
code+="""
#define ORIGAMI_ASYNC_VIDEO_PLAYBACK 1
struct {template<class... T>void printf(const char*,T...) {}} Serial;
namespace services {namespace camera {struct PreviewDecode {
 struct Frame{const uint16_t* pixels;uint64_t sensorUs;uint32_t decodeUs;};
 bool startup=true,bad=false,pending=false,borrowed=false;uint32_t ready=0;uint64_t stamp=0;
 bool begin(bool fit){assert(fit);return startup;} bool failed(){return bad;}
 bool submit(const uint8_t*,size_t,uint64_t time){assert(!pending&&!borrowed);pending=true;stamp=time;ready=clockMs+70;return true;}
 bool acquire(Frame& f){if(!pending||int32_t(clockMs-ready)<0)return false;pending=false;borrowed=true;static uint16_t pixel;f={&pixel,stamp,70000};return true;}
 void release(){assert(borrowed);borrowed=false;} void close(){assert(!borrowed);pending=false;}
};}}
static services::camera::PreviewDecode s_decode;
static bool s_decodeTried=false,s_asyncDecode=false,s_decodePending=false;
static uint32_t s_pendingNumber=0,s_pendingImage=0;
"""
code+='\n'.join(function(s) for s in ['static void resetDecode(', 'static uint32_t playbackTimeMs(', 'static bool setPlaying(', 'static void failPlayback(', 'static bool finishIfDone(', 'static void tickAsync(', 'static void tick('])
code+=r'''
void reset(){resetDecode();clockMs=0;s_library={};s_preview={};s_decode={};s_frame=s_playing=s_ended=s_failed=false;s_audioTail=false;s_number=s_elapsedMs=s_skipped=s_decoded=s_imageNumber=s_presentedMs=0;displayed.clear();}
void step(){tick(clockMs);assert(!s_library.borrowed&&!s_decode.borrowed);clockMs+=5;}
void preview(){for(int i=0;i<100&&!s_frame&&!s_failed;++i)step();assert(s_frame&&!s_playing);}
int main(){
 for(auto audio:{0u,384000u}){
  reset();s_library.metadata.audioSamples=audio;preview();assert(s_number==0);
  clockMs+=1000;step();assert(s_number==0);assert(setPlaying(true));
  for(int i=0;i<1000&&!s_decodePending;++i)step();assert(s_decodePending);
  setPlaying(false);auto frame=s_number,paused=playbackTimeMs(clockMs);clockMs+=2000;step();
  assert(s_number==frame&&playbackTimeMs(clockMs)==paused);setPlaying(true);
  for(int i=0;i<5000&&!s_ended;++i)step();
  assert(s_ended&&s_number==199&&!s_playing);assert(displayed.front()==0&&displayed.back()==199);
  assert(s_decoded+s_skipped==200);assert(!s_decodePending);
 }
 reset();s_decode.startup=false;preview();assert(!s_asyncDecode&&s_frame);
 reset();step();assert(s_decodePending);s_decode.bad=true;step();assert(s_failed&&!s_decodePending&&!s_frame);
 reset();step();assert(s_decodePending);resetDecode();assert(!s_decode.pending&&!s_decodePending);s_library={};preview();
 reset();s_library.metadata.frames=1;s_library.metadata.intervalUs=1000000;preview();assert(!s_ended);setPlaying(true);
 for(int i=0;i<300&&!s_ended;++i)step();assert(s_ended);
 reset();s_library.holdSpan=25;preview();setPlaying(true);for(int i=0;i<5000&&!s_ended;++i)step();assert(s_ended&&s_decoded==8);
 reset();s_decode.startup=false;s_preview.fail=true;step();assert(s_failed&&!s_asyncDecode);
 reset();step();++s_decode.stamp;for(int i=0;i<100&&!s_failed;++i)step();assert(s_failed&&!s_decodePending);
 reset();s_preview.fail=true;for(int i=0;i<100&&!s_failed;++i)step();assert(s_failed&&!s_decodePending);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS asynchronous playback: delayed decode, pause, EOF/audio, holds, failure, fallback and canceled re-entry')
