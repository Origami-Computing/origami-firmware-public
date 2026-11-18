"""Exercise the sole Camera's actual workflow with faultable device adapters."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/apps/cine_camera_app.cpp').read_text(encoding='utf8')

def function(signature):
    a = source.index(signature)
    b = source.index('{', a) + 1
    depth = 1
    while depth:
        depth += (source[b] == '{') - (source[b] == '}')
        b += 1
    return source[a:b]

assert not (ROOT / 'src/apps/camera_menu_app.cpp').exists()
assert not (ROOT / 'include/apps/camera_menu_app.h').exists()
for path in ['src/apps/menu_app.cpp', 'src/apps/agent/screen_eyes.cpp',
             'src/apps/tutorial_app.cpp', 'src/tooling/dev_serial_input.cpp']:
    text = (ROOT / path).read_text(encoding='utf8')
    assert 'cine_camera' in text and 'camera_menu' not in text, path

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "core/cine_controls.h"
std::string trace;
void advanceShutter();
uint32_t clockMs=0;void delay(unsigned ms){clockMs+=ms;}uint32_t millis(){return clockMs;}uint32_t micros(){return clockMs*1000;}
bool soundOk=true,startOk=true,captureOk=true;
struct TFT_eSprite{};
namespace hal {namespace display {
TFT_eSprite& canvas(){static TFT_eSprite c;return c;}
extern bool paintRecorded;void flush(){if(paintRecorded)trace+="flush;";}
} namespace input {
enum class EventType{ButtonB_Press,ButtonB_Release,ButtonB_Tap,ButtonA_Press,ButtonC_Press,Other};
struct InputEvent{EventType type;};bool a=false,b=false,c=false;
bool buttonADown(){return a;}bool buttonBDown(){return b;}bool buttonCDown(){return c;}
}namespace camera{struct CineIspRequest{int color=3;};}}
struct {template<class... T>void printf(const char*,T...){} }Serial;
namespace sfx{using PcmClip=int;const int kCameraShutter=0,kRecordingStart=1,kRecordingStop=2;
bool playAndRelease(int cue,void(*progress)()){assert(trace.size()>=6&&trace.substr(trace.size()-6)=="flush;");
trace+=cue==0?"photo;":cue==1?"start;":"stop;";if(soundOk)for(int i=0;i<8;++i){clockMs+=23;progress();}return soundOk;}}
namespace services {namespace camera {
enum class CaptureResolution{Vga,Hd};
struct VideoRecording{
 enum class State{Saved,Failed};static constexpr unsigned kMaxDurationMs=30000;
 bool active=false,saved=true;unsigned stops=0,closes=0;
 bool busy(){return active;}void stop(){++stops;trace+="stop-request;";}
 void close(){++closes;active=false;trace+="close;";}
 State state(){return saved?State::Saved:State::Failed;}
 struct Stats{unsigned unique=3,maxChunkWriteMs=1,maxQueued=1;};
 Stats captureStats(){return{};}Stats writerStats(){return{};}Stats submissionStats(){return{};}
 unsigned frames(){return 3;}unsigned repeatedFrames(){return 0;}unsigned audioSamples(){return 100;}unsigned dropped(){return 0;}
 const char* path(){return "/clip.avi";}
};}
namespace photos{struct Target{unsigned sequence=0,capturedAtUnix=0;char path[64]={};};}
namespace uploads{bool ensureInit(){return true;}struct Queue{void enqueueVideo(const char*){trace+="queue;";}};Queue& queue(){static Queue q;return q;}}
}
services::camera::VideoRecording recorder;
struct {services::camera::CaptureResolution capture=services::camera::CaptureResolution::Vga;}cameraSettings;
struct Session{
 struct Status{bool queued=false;unsigned sequence=5,capturedAtUnix=42;char photoPath[64]="/photo.jpg";}s;
 hal::camera::CineIspRequest captureSettings(){return{};}
 bool beginClip(services::camera::VideoRecording& r,bool hd,bool mic){trace+="acquire;";lastHd=hd;lastMic=mic;r.active=startOk;return startOk;}
 bool lastHd=false,lastMic=false;
 bool capture(void(*feedback)(),bool enqueue){s.queued=false;trace+="expose;";if(!captureOk)return false;feedback();trace+="write;";s.queued=enqueue;return true;}
 const Status& status(){return s;}bool open(){trace+="open;";return true;}
 bool restoreSettings(hal::camera::CineIspRequest){trace+="restore;";return true;}
 bool previewClip(services::camera::VideoRecording&){return false;}
 bool preview(bool(*)(void*)){return false;}void close(){trace+="sensor-close;";}
}session;
enum class View{Menu,Opening,Preview,Error};View current=View::Preview;
bool microphone=true,clipActive=false,stopping=false,transitionPulse=false,tutorialCapture=false;
bool shutterHeld=false,cancelTap=false,restoreAfterClip=false,freshPreview=false;
uint32_t clipStartedAt=0,shutterPressedAt=0,feedbackStage=0,feedbackAt=0,lastPreview=0,previewDecodeUs=0;
hal::camera::CineIspRequest clipSettings;core::cine::WheelControl wheel;
services::photos::Target tutorialPhoto;
bool transitionFlushPending=false;
uint32_t transitionAt=0,transitionPaintAt=0,transitionPaintMaxUs=0;
namespace hal{namespace display{bool paintRecorded=true;}}
bool renderingCue=false;
void invalidate(){}void draw(TFT_eSprite&){
 hal::display::paintRecorded=true;
 if(transitionPulse){if(!renderingCue)trace+="white;";else hal::display::paintRecorded=false;renderingCue=true;}
 else if(renderingCue){renderingCue=false;trace+="clear;";assert(uint32_t(millis()-transitionAt)>=140);}
 else trace+="draw;";
}
bool previewShouldAbort(void*){return false;}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}void pop(){session.close();trace+="pop;";}};
'''
for sig in ['void advanceShutter()', 'void presentCue(', 'void photoFeedback()', 'void snap()', 'bool finishClip()',
            'void restoreStill(', 'bool beginVideo()', 'void tick(uint32_t)']:
    code += function(sig) + '\n'
code += r'''
void input(hal::input::InputEvent e){if(e.type==hal::input::EventType::ButtonB_Tap)snap();}
'''+function('void shutterInput(')+r'''
void reset(){trace.clear();clockMs=100;current=View::Preview;clipActive=stopping=shutterHeld=cancelTap=tutorialCapture=false;
 recorder={};hal::input::a=hal::input::b=hal::input::c=false;startOk=soundOk=captureOk=true;feedbackStage=0;}
void press(){hal::input::b=true;shutterInput({hal::input::EventType::ButtonB_Press});}
void release(){hal::input::b=false;shutterInput({hal::input::EventType::ButtonB_Release});shutterInput({hal::input::EventType::ButtonB_Tap});}
int main(){
 for(bool audio:{false,true})for(bool hd:{false,true})for(bool sound:{false,true}){
  reset();microphone=audio;soundOk=sound;cameraSettings.capture=hd?services::camera::CaptureResolution::Hd:services::camera::CaptureResolution::Vga;
  press();clockMs+=1199;tick(0);assert(!clipActive&&trace.empty());++clockMs;tick(0);
  assert(clipActive&&trace=="white;flush;start;clear;flush;acquire;"&&session.lastHd==hd&&session.lastMic==audio);
  release();assert(clipActive&&!recorder.stops);press();release();assert(recorder.stops==1&&stopping);
  recorder.active=false;tick(0);assert(!clipActive);
  assert(trace=="white;flush;start;clear;flush;acquire;stop-request;close;queue;white;flush;stop;clear;flush;open;restore;");
  trace.clear();tick(0);assert(trace.empty());
 }
 reset();startOk=false;press();clockMs+=1200;tick(0);release();assert(!clipActive);
 assert(trace=="white;flush;start;clear;flush;acquire;close;open;restore;");
 reset();press();hal::input::a=true;clockMs+=1200;tick(0);release();assert(trace.empty()&&!clipActive);
 reset();press();clockMs+=200;release();assert(trace=="expose;white;flush;photo;clear;flush;write;draw;flush;");
 reset();captureOk=false;press();release();assert(trace=="expose;draw;flush;");
 reset();tutorialCapture=true;press();clockMs+=1300;tick(0);assert(!clipActive);release();
 assert(trace=="expose;white;flush;photo;clear;flush;write;sensor-close;pop;");
 assert(tutorialPhoto.sequence==5&&tutorialPhoto.capturedAtUnix==42&&!strcmp(tutorialPhoto.path,"/photo.jpg"));
 reset();assert(beginVideo());recorder.saved=false;recorder.active=false;trace.clear();tick(0);
 assert(trace=="close;open;restore;"); // Failed recordings do not announce a saved clip.
}
'''
with tempfile.TemporaryDirectory(prefix='camera-workflow-') as td:
    p = Path(td)
    (p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',
                    '-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS sole Camera: actual hold/release, photo/tutorial, synchronized cues, VGA/HD + mic, sound/start/save failures and close-before-stop playback; ASan/UBSan')
