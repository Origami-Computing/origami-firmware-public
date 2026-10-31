"""Exercise actual Camera mode routing and view callbacks with operation fakes."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/apps/camera_menu_app.cpp').read_text()
def function(signature):
    start=source.index(signature+' {');opening=source.index('{',start)
    end=opening+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    code=r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cstdint>
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
uint32_t now=0;uint32_t millis(){return now;}
bool s_waitingForNotice=false;uint32_t s_noticeWaitStarted=0;
namespace services {namespace firmware_update {bool active=false;bool noticeInFlight(){return active;}}}
namespace ui {void showAppOpeningSplash(const char*){}}
struct SerialFake {void println(const char*){}}Serial;
enum State {CAM_INIT,CAM_VIEWFINDER,CAM_SETTINGS,CAM_MANUAL,CAM_README,CAM_ERROR};
State camState=CAM_INIT;
char camStatusMsg[80]={};
bool startOk=true, remainsRunning=false;
struct Video {bool active=false;int closes=0;bool busy(){return active;}
 void close(){active=false;++closes;}}s_video;
int starts=0,stops=0,restores=0,published=0,errorDraws=0;
int settings=0,manual=0,readme=0;
bool s_settingsNeedsRedraw=false,s_manualNeedsRedraw=false,s_readmeNeedsRedraw=false;
namespace services {namespace camera {struct Session {enum class Resume{Deferred,DeferredWithInputRecovery};};}}
struct Camera {bool running(){return remainsRunning;}
void restoreUi(services::camera::Session::Resume){++restores;}}s_camera;
bool camStartViewfinder(){assert(!s_video.busy());++starts;return startOk;}
void camStopViewfinder(){++stops;}
void camReleasePreviewBuffers(){}
void camPublishViewfinderScene(bool){++published;}
void camDrawError(){++errorDraws;}
void prepareSettingsMode(){++settings;camState=CAM_SETTINGS;}
void prepareManualMode(){++manual;camState=CAM_MANUAL;}
void prepareReadmeMode(){++readme;camState=CAM_README;}
static void enterViewfinderScreen();static void enterSettingsScreen();
static void enterManualScene();static void enterReadmeScene();static void enterErrorScreen();
struct Screen {void(*onEnter)();};
const Screen kScreenCameraViewfinder{enterViewfinderScreen},kScreenCameraSettings{enterSettingsScreen},
kScreenCameraManual{enterManualScene},kScreenCameraReadme{enterReadmeScene},kScreenCameraError{enterErrorScreen};
struct App {const Screen* currentScreen;}app{&kScreenCameraViewfinder};
namespace scene {void switchTo(App*a,const Screen*s){a->currentScreen=s;s->onEnter();}}
static void showError();
'''
    for sig in ['static bool requestViewfinder()', 'static void servicePendingViewfinder()',
                'static void camExit()', 'static void resumeCameraWorkflow()',
                'static void enterViewfinderScreen()', 'static void enterSettingsScreen()',
                'static void enterManualScene()', 'static void enterReadmeScene()',
                'static void enterErrorScreen()', 'static void showViewfinder()',
                'static void showSettings()', 'static void showManual()',
                'static void showReadme()', 'static void showError()']:
        code+=function(sig)+'\n'
    code+=r'''
int main(){
// Refreshing every view must not request camera startup or cleanup.
enterViewfinderScreen();enterSettingsScreen();enterManualScene();enterReadmeScene();enterErrorScreen();
assert(starts==0&&stops==0&&restores==0&&settings==0&&manual==0&&readme==0);
assert(published==1&&errorDraws==1&&s_settingsNeedsRedraw&&s_manualNeedsRedraw&&s_readmeNeedsRedraw);
showSettings();assert(settings==1&&app.currentScreen==&kScreenCameraSettings);
resumeCameraWorkflow();assert(settings==2&&starts==0);
showManual();resumeCameraWorkflow();assert(manual==2&&app.currentScreen==&kScreenCameraManual);
showReadme();resumeCameraWorkflow();assert(readme==2&&app.currentScreen==&kScreenCameraReadme);
showViewfinder();assert(starts==1&&camState==CAM_VIEWFINDER&&app.currentScreen==&kScreenCameraViewfinder);
s_video.active=true;resumeCameraWorkflow();assert(starts==2&&s_video.closes==1&&!s_video.busy());
enterViewfinderScreen();assert(starts==2);
// A failed mode request stays on Error and releases/restores through the same path.
startOk=false;
for(State from:{CAM_INIT,CAM_VIEWFINDER,CAM_SETTINGS,CAM_MANUAL,CAM_README}){
 camState=from;app.currentScreen=&kScreenCameraViewfinder;strcpy(camStatusMsg,"Upload in progress");
 int before=starts,freed=stops,resumed=restores;
 showViewfinder();assert(starts==before+1&&stops==freed+1&&restores==resumed+1);
 assert(camState==CAM_ERROR&&app.currentScreen==&kScreenCameraError);
 assert(!strcmp(camStatusMsg,from==CAM_SETTINGS||from==CAM_MANUAL||from==CAM_README?"Camera init failed":"Upload in progress"));
 resumeCameraWorkflow();enterErrorScreen();assert(starts==before+1);
}
remainsRunning=true;int before=restores;showError();assert(restores==before);
remainsRunning=false;startOk=true;showViewfinder();assert(camState==CAM_VIEWFINDER);
// In-flight optional metadata defers startup without consuming camera resources.
services::firmware_update::active=true;now=100;before=starts;
showViewfinder();assert(s_waitingForNotice&&camState==CAM_INIT&&starts==before);
now=29999;servicePendingViewfinder();assert(starts==before&&s_waitingForNotice);
services::firmware_update::active=false;servicePendingViewfinder();
assert(starts==before+1&&!s_waitingForNotice&&camState==CAM_VIEWFINDER);
// Leaving/covering cancels the pending request; later completion cannot restart it.
services::firmware_update::active=true;showViewfinder();camExit();before=starts;
services::firmware_update::active=false;servicePendingViewfinder();assert(starts==before&&!s_waitingForNotice);
// A bounded wait offers normal Error/Back/Retry; wraparound keeps the same bound.
services::firmware_update::active=true;now=0xfffffff0u;showViewfinder();before=starts;
now+=29999u;servicePendingViewfinder();assert(s_waitingForNotice);
now+=1u;servicePendingViewfinder();assert(!s_waitingForNotice&&camState==CAM_ERROR&&starts==before);
services::firmware_update::active=false;showViewfinder();assert(camState==CAM_VIEWFINDER&&starts==before+1);
}
'''
    (root/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS actual Camera view/mode callbacks: draw-only entry, explicit transitions/resume, failed-start routing and guarded network restoration')
