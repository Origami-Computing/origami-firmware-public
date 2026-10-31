"""Compile production shutter workflow with controlled input and recording outcomes."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/apps/camera_menu_app.cpp').read_text()
def function(signature):
    a = source.index(signature); b = source.index('{', a)+1; depth = 1
    while depth:
        depth += (source[b]=='{')-(source[b]=='}'); b += 1
    return source[a:b]
state = source[source.index('static constexpr uint32_t kVideoHoldMs'):source.index('// UI-owned pipeline measurements')]
code = r'''
#include "app.h"
#include <cassert>
#include <cstdint>
namespace hal { namespace input {
enum class EventType {ButtonB_Press,ButtonB_Release,ButtonB_Tap,ButtonA_Press,ButtonC_Press};
struct InputEvent {EventType type;};
bool a=false,b=false,c=false;
bool buttonADown(){return a;} bool buttonBDown(){return b;} bool buttonCDown(){return c;}
}}
using ET=hal::input::EventType;
constexpr int kDefaultShootMode=0,CAM_VIEWFINDER=1;
int s_shootMode=0,camState=CAM_VIEWFINDER;
bool s_tutorialCapture=false,s_videoPending=false,s_reviewActive=false,s_continuousVga=true,s_videoStopping=false;
struct {bool active=false;bool busy(){return active;} void close(){active=false;}}s_video;
uint32_t now=0;uint32_t millis(){return now;}
int starts=0;bool startOk=true,pendingOnFailure=false;
bool camBeginVideo(){assert(s_shootMode==2&&!s_continuousVga);++starts;
 s_videoPending=startOk||pendingOnFailure;s_video.active=startOk;return startOk;}
void camPresentPhotographer(){}

int chromeInvalidations=0;bool direct=true;
namespace hal {namespace display {void exitOptimizedRender(){direct=false;}}}
struct NavHandler {static NavHandler& instance(){static NavHandler n;return n;}
 void runtimePolicyChanged(){} void invalidateChrome(){assert(!direct);++chromeInvalidations;}};
struct {void stop(){assert(!s_video.active);}}s_camera;
struct {void println(const char*){}}Serial;
void delay(unsigned){} void camReleasePreviewBuffers(){} void camReportVideoResult(){}
''' + state + function('static void camStopViewfinder()') + function('static bool camShutterInput(') + function('static void camTickShutter()') + r'''
bool event(ET e){return camShutterInput({e});}
void press(){hal::input::b=true;event(ET::ButtonB_Press);}
bool release(){hal::input::b=false;event(ET::ButtonB_Release);return event(ET::ButtonB_Tap);}
void reset(){s_videoPending=false;s_video.active=false;camFinishQuickVideo();s_shutterSkipTap=false;
 hal::input::a=hal::input::b=hal::input::c=false;startOk=true;pendingOnFailure=false;}
int main(){
 for(unsigned duration:{0u,100u,1199u}){reset();press();now+=duration;camTickShutter();
 assert(!release()&&starts==0&&s_shootMode==0);}
 press();now+=1200;camTickShutter();assert(starts==1&&s_quickVideo&&s_video.active);
 now+=5000;camTickShutter();assert(starts==1);assert(release()&&s_video.active);
 press();assert(!s_shutterArmed);assert(!release()); // next tap reaches normal stop
 reset();assert(s_shootMode==0);
 // C or A during a hold cancels both video and its trailing photo tap.
 for(bool a:{true,false}){press();if(a)hal::input::a=true;else hal::input::c=true;
 now+=1300;camTickShutter();assert(starts==1&&release());reset();}
 press();event(ET::ButtonC_Press);assert(release()&&starts==1);reset();
 // Cover/exit cleanup disarms: keeping B down cannot start on resume.
 press();camFinishQuickVideo();now+=2000;camTickShutter();assert(starts==1);reset();
 // Timer wrap and both synchronous/async startup failures.
 now=UINT32_MAX-500;press();now+=1200;startOk=false;camTickShutter();
 assert(starts==2&&!s_quickVideo&&s_shootMode==0&&release());reset();
 press();now+=1200;startOk=false;pendingOnFailure=true;camTickShutter();
 assert(s_quickVideo&&s_videoPending&&release());reset();
 for(int mode:{1,2}){s_shootMode=mode;press();now+=1300;camTickShutter();assert(!release()&&starts==3);}
 reset();s_quickVideo=true;s_shootMode=2;s_video.active=true;s_videoPending=true;
 camStopViewfinder();assert(chromeInvalidations==1&&!s_videoPending&&!s_quickVideo&&s_shootMode==0);
 reset();s_tutorialCapture=true;press();now+=1300;camTickShutter();assert(!release()&&starts==3);
}
'''
code = '#include <initializer_list>\n'+code
with tempfile.TemporaryDirectory() as directory:
    cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(code)
    subprocess.run(['c++','-I'+str(root/'include'),'-std=c++17','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS production shutter: tap, hold latch, release suppression, next tap, chord/cancel, cleanup, wrap, failures, tutorial/settings')
