"""Exercise production Videos scheduling with a slow renderer and bounded prefetch."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/apps/videos_app.cpp').read_text()
def function(signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
code=r'''
#include "core/mjpeg_avi.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
uint32_t prefetchSlots=3;
uint32_t clockMs=0;uint32_t millis(){return clockMs;}
namespace hal {namespace display {void exitOptimizedRender(){}void enterOptimizedRender(){}void markAllDirty(){}}}
struct TFT_eSprite {}; int app=0; const void* activeApp=nullptr;
struct NavHandler {const void* top(){return activeApp;}static NavHandler& instance(){static NavHandler n;return n;}void runtimePolicyChanged(){}};
namespace services {namespace video {
struct Library {
 enum class State {Idle,Loading,Ready,Failed};
 struct Frame{const uint8_t* data=nullptr;uint32_t length=1,number=0,imageNumber=0;};
 struct Info {uint32_t frames=200,intervalUs=40000,audioSamples=0;} metadata;
 uint32_t next=0,takes=0,releases=0;bool borrowed=false,fail=false,loading=false;uint32_t holdSpan=1,availableThrough=UINT32_MAX;
 int count(){return 1;}State state(){return fail?State::Failed:loading?State::Loading:State::Ready;}
 const char* error(){return "failed";}void stop(){assert(!borrowed);}
 const Info& info(){assert(!loading);return metadata;}
 bool audioPlaying=false,audioStarted=false;uint32_t audioMs=0,audioStart=0;
 bool startAudio(){if(!audioPlaying){audioStart=clockMs;audioPlaying=audioStarted=true;}return true;}
 uint32_t audioPositionMs()const{return std::min(uint32_t(uint64_t(metadata.audioSamples)*1000/48000),audioMs+(audioPlaying?uint32_t(clockMs-audioStart):0));}
 void pauseAudio(){audioMs=audioPositionMs();audioPlaying=false;}
 bool audioFailed()const{return false;}
 bool waiting=false;bool opening()const{return waiting;}
 bool take(Frame& f,uint32_t target=UINT32_MAX){++takes;assert(!borrowed);if(next==metadata.frames || next>availableThrough)return false;if(target!=UINT32_MAX){if(next>target)return false;next=std::min(std::min(std::min(target,metadata.frames-1),availableThrough),next+prefetchSlots-1);}
 borrowed=true;f.number=next;f.imageNumber=(next/holdSpan)*holdSpan;return true;}
 void release(){assert(borrowed);borrowed=false;++next;++releases;}
};}}
namespace ui {struct CameraPreview {
 enum class Result{Decoded,Invalid};bool fail=false;uint32_t decodes=0,captures=0;
 bool copyCompositeForDevelopment(TFT_eSprite&){++captures;return !fail;}
 Result decodeVideo(const uint8_t*,uint32_t){++decodes;clockMs+=70;return fail?Result::Invalid:Result::Decoded;}
};}
static services::video::Library s_library;
static ui::CameraPreview s_preview;
static bool s_frame=false,s_playing=false,s_ended=false,s_dirty=true,s_failed=false;
static bool s_audioTail=false;
static uint32_t s_number=0,s_startedMs=0,s_elapsedMs=0,s_skipped=0,s_decodeMaxMs=0,s_presentMaxMs=0;
static uint32_t s_imageNumber=0,s_presentedMs=0,s_decoded=0,s_held=0;
static uint32_t s_tickExit=0,s_tickOutside=0,s_tickWork=0,s_tickCalls=0,s_tickGapMax=0;
static const char* s_message="";
unsigned openPolls=0;static void open(){++openPolls;}
std::vector<uint32_t> displayed;
static void present(){displayed.push_back(s_number);clockMs+=50;s_presentedMs=clockMs;}
'''
code+='\n'.join(function(sig) for sig in ['static uint32_t playbackTimeMs(', 'static bool setPlaying(', 'static void failPlayback(', 'static bool finishIfDone(', 'static void tick(', 'bool copyPreviewForDevelopment('])
code+=r'''
void reset(uint32_t now){
 clockMs=now;openPolls=0;s_library={};s_preview={};s_frame=s_playing=s_ended=s_failed=false;
 s_number=s_startedMs=s_elapsedMs=s_skipped=s_imageNumber=s_presentedMs=s_decoded=s_held=0;s_audioTail=false;displayed.clear();
 s_tickExit=s_tickOutside=s_tickWork=s_tickCalls=s_tickGapMax=0;
}
void step(){uint32_t before=s_library.takes;tick(clockMs);assert(s_library.takes-before<=3);assert(!s_library.borrowed);clockMs+=5;}
void check(){
 reset(0);s_library.waiting=true;step();assert(openPolls==1 && !s_library.takes);
 reset(0);s_library.loading=true;step();assert(!s_frame&&!s_failed&&s_library.takes==0);
 s_library.loading=false;step();assert(s_frame&&!s_failed);
 for(uint32_t audioSamples:{0u,384000u,192000u}) for(uint32_t initial:{0u,0xffffff00u}) {
  reset(initial);s_library.metadata.audioSamples=audioSamples;step();assert(s_frame&&!s_playing&&s_number==0);
  clockMs+=5000;step();assert(s_number==0&&playbackTimeMs(clockMs)==0);
  setPlaying(true);uint32_t started=clockMs;
  for(int n=0;n<10;++n)step();assert(s_number>0);
  setPlaying(false);uint32_t paused=playbackTimeMs(clockMs),frame=s_number;
  clockMs+=2000;step();assert(s_number==frame&&playbackTimeMs(clockMs)==paused);
  setPlaying(true);
  for(int n=0;n<1000&&!s_ended;++n)step();
  assert(s_ended&&!s_playing&&s_number==199&&s_skipped>100);
  // Eight-second media remains eight seconds, excluding the explicit pause,
  // even though each displayed frame costs 120 ms and the clock wraps.
  uint32_t duration=clockMs-started-2005;
  assert(duration>=7800&&duration<=8300);
  assert(displayed.front()==0&&displayed.back()==199);
  assert(displayed.size()+s_skipped==200);
  uint32_t elapsed=playbackTimeMs(clockMs);clockMs+=1000;step();assert(playbackTimeMs(clockMs)==elapsed);
 }
 // A producer two intervals behind the clock must still make visual progress.
 // The former exact-target loop discarded every available image until EOF.
 reset(0);step();assert(setPlaying(true));uint32_t lagStarted=clockMs;
 for(int n=0;n<2500&&!s_ended;++n){
  uint32_t elapsed=clockMs-lagStarted;
  s_library.availableThrough=elapsed>=80?std::min(199u,(elapsed-80)/40):0;
  step();
 }
 assert(s_ended&&s_preview.decodes>30&&clockMs-lagStarted<8500);
 // Holds advance the audio clock and end state without redundant decoding.
 reset(0);s_library.metadata.audioSamples=384000;s_library.holdSpan=25;
 step();assert(s_preview.decodes==1);setPlaying(true);
 for(int n=0;n<2500&&!s_ended;++n)step();
 assert(s_ended && s_number==199 && s_preview.decodes==8);
 assert(displayed.size()<35);
 reset(0);s_preview.fail=true;step();assert(s_failed&&!s_playing&&!s_frame);
 reset(0);s_library.fail=true;step();assert(s_failed&&!s_playing&&!s_frame);
 // A silent still-frame video has a real duration, including after a pause.
 // It must not be marked ended merely by decoding its initial preview.
 for(uint32_t initial:{0u,0xffffff00u}) {
  reset(initial);s_library.metadata.frames=1;s_library.metadata.intervalUs=1000000;
  step();assert(s_frame&&!s_ended&&!s_playing);
  clockMs+=5000;step();assert(!s_ended&&playbackTimeMs(clockMs)==0);
  assert(setPlaying(true));uint32_t started=clockMs;
  clockMs=started+999;tick(clockMs);assert(!s_ended);
  setPlaying(false);clockMs+=2000;tick(clockMs);assert(!s_ended);
  setPlaying(true);clockMs+=1;tick(clockMs);assert(s_ended&&!s_playing);
 }
}
int main(){for(auto slots:{3u,6u}){prefetchSlots=slots;check();}
 reset(0);TFT_eSprite target;
 assert(!copyPreviewForDevelopment(target)&&s_preview.captures==0);
 activeApp=&app;assert(!copyPreviewForDevelopment(target)&&s_preview.captures==0);
 s_frame=true;s_playing=true;
 assert(!copyPreviewForDevelopment(target)&&s_preview.captures==0);
 s_playing=false;assert(copyPreviewForDevelopment(target)&&s_preview.captures==1);
 s_failed=true;assert(!copyPreviewForDevelopment(target)&&s_preview.captures==1);
 s_failed=false;s_preview.fail=true;assert(!copyPreviewForDevelopment(target)&&s_preview.captures==2);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS production playback timing: slow rendering, bounded late-frame skipping, pause/resume, clock wrap and failures')
