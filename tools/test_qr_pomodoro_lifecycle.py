"""Compile actual workflow/view functions and descriptors with fault-free boundaries."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    match=re.search(r'(?:static )?void '+name+r'\([^)]*\)\s*\{',source)
    assert match,name
    end=match.end();depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]

def declaration(source,prefix):
    start=source.index(prefix);return source[start:source.index(';',start)+1]

common=r'''
#include "app.h"
#include <cassert>
#include <cstring>
#include <cstdio>
namespace scene {struct Screen {
 const char* name;void(*onEnter)();void(*onExit)();void(*onTick)(uint32_t);
 void(*onInput)(int);bool(*onChord)();void(*onDraw)(TFT_eSprite&);
 const char* id;void(*buildScene)();
 const uint16_t* statusBarBackground;uint16_t(*statusBarBackgroundNow)();
};}
unsigned now=1234,draws=0;
unsigned millis(){return now;}
void onInput(int){} bool onChord(){return true;}
AppPresentation presentationPolicy(){return {};}
core::health::RuntimePolicy runtimePolicy(){return {};}
namespace hal {namespace display {void markAllDirty(){++draws;}}}
'''
qr=r'''
enum class State { PREPARING,SCANNING,LISTENING,RELEASING,CONNECTING,SUCCESS,ERROR };
State s_state=State::ERROR;
bool s_audioMode=false,s_restartPending=false;unsigned s_errorSelection=0;
char lastError[96]={};
void showError(const char* reason,const char* title){s_state=State::ERROR;snprintf(lastError,sizeof(lastError),"%s: %s",title,reason);}
struct {void clear(){}} s_audioAssembly;
char s_audioError[64];unsigned s_audioPaint=0,s_audioStart=0;
unsigned audioStarts=0;bool audioStartOk=true,noticePending=false;
namespace services {namespace ggwave_receiver {bool start(bool){++audioStarts;return audioStartOk;}void stop(){}}
namespace firmware_update {bool noticeInFlight(){return noticePending;}}}
void copyField(const char* a,char* b,unsigned n){strncpy(b,a,n);}
bool s_tutorialMode=true,s_chromeDrawn=true,s_displayBypass=true,s_needsScanRequest=false;
uint32_t s_connectStartMs=9,s_scanAttempts=9,s_frames=9,s_prepareStartMs=9,s_lastProgressMs=9;
char s_credentials[32],s_statusLine[64],s_detailLine[64],s_lastPayload[256];
struct {void println(const char*){}} Serial;
unsigned closes=0,startingViews=0;
struct Preview { void close(){} } s_preview;
struct Session { bool close(void(&retire)()){++closes;retire();return true;} } s_session;
void setDisplayBypass(bool on){s_displayBypass=on;}
void drawStatusScreen(const char* title,const char* detail,const char* back,bool ok){
 if(s_audioMode)assert(!strcmp(title,"Starting audio...")&&!strcmp(detail,"Please wait")&&!back&&!ok);
 else assert(!strcmp(title,"Starting scanner...")&&!detail&&!strcmp(back,"Left: back")&&!ok);
 ++startingViews;
}
void screenTick(uint32_t){}void buildScannerPreviewScene(){}
uint16_t backgroundColor(){return 0;} // Presentation is outside this lifecycle check.
'''
qr_main=r'''
int main(){
 assert(app.onEnter&&app.onResume&&app.onExit&&!app.initialScreen->onExit);
 memset(s_credentials,7,sizeof(s_credentials));app.onEnter();
 assert(s_state==State::PREPARING&&s_needsScanRequest&&!s_frames&&!s_scanAttempts&&!s_connectStartMs);
 assert(s_prepareStartMs==now&&s_lastProgressMs==now&&!s_credentials[0]&&!closes&&!startingViews);
 // View refresh cannot reset workflow, consume credentials, or close the session.
 s_state=State::CONNECTING;s_connectStartMs=400;s_frames=5;s_credentials[0]=7;s_needsScanRequest=false;
 app.initialScreen->onEnter();
 assert(s_state==State::CONNECTING&&s_connectStartMs==400&&s_frames==5&&s_credentials[0]==7&&!s_needsScanRequest&&!closes&&startingViews==1);
 app.onResume();assert(s_state==State::PREPARING&&!s_credentials[0]&&s_needsScanRequest&&s_tutorialMode);
 app.initialScreen->onEnter();assert(startingViews==2&&!closes);
 app.onExit();assert(closes==1&&!s_tutorialMode&&!s_displayBypass&&!s_chromeDrawn&&!s_credentials[0]);
 s_audioMode=true;noticePending=true;app.onEnter();app.initialScreen->onEnter();
 tickAudioPreparation(now+100);assert(s_state==State::PREPARING&&!audioStarts);
 // Cancelling before metadata finishes must not acquire audio/radio resources.
 app.onExit();assert(!s_audioMode&&!audioStarts);
 s_audioMode=true;app.onEnter();tickAudioPreparation(now+200);
 noticePending=false;tickAudioPreparation(now+300);
 assert(s_state==State::LISTENING&&audioStarts==1&&s_audioStart==now+300&&!s_audioError[0]);
 app.onExit();s_audioMode=true;noticePending=true;app.onEnter();
 tickAudioPreparation(now+30000);assert(s_state==State::ERROR&&audioStarts==1&&strstr(lastError,"Network busy"));
 app.onExit();s_audioMode=true;noticePending=false;audioStartOk=false;app.onEnter();
 tickAudioPreparation(now);assert(s_state==State::ERROR&&audioStarts==2&&strstr(lastError,"Microphone unavailable"));app.onExit();
 puts("PASS QR descriptor activation/resume restart, isolated view entry and app-owned close");
 puts("PASS audio notice wait, pre-start cancellation, automatic startup, bounded timeout and receiver failure");
}
'''
pom=r'''
enum class Mode {Setup,Running,Paused,Complete,Settings};enum Field{Focus,Break,Sound,Vibration,FieldCount};
Mode s_mode=Mode::Complete;Field s_field=Sound;
bool s_active=false,s_dirty=false,s_break=true,s_editing=true;
unsigned s_animationAt=0,s_duration=0,s_remaining=0,loads=0,saves=0,stops=0,previews=0;
namespace services {namespace pomodoro {struct Settings {
 uint8_t focusMinutes=25,breakMinutes=5,sound=0,vibration=0;
 void load(){loads+=4;focusMinutes=99;breakMinutes=1;sound=3;vibration=2;}
 bool save()const{++saves;return true;}
};}namespace vibration {void stopPreview(){++previews;}}}
services::pomodoro::Settings s_settings;
void stopFeedback(){++stops;}
void tick(uint32_t){}void onDraw(TFT_eSprite&){}void buildPomodoroScene(){}
'''
pom_main=r'''
int main(){
 assert(app.onEnter&&!app.onResume&&app.onExit&&!app.initialScreen->onExit);
 app.onEnter();assert(s_active&&loads==4&&draws==0&&s_mode==Mode::Setup&&s_field==Focus&&!s_break&&!s_editing);
 assert(s_settings.focusMinutes==99&&s_settings.breakMinutes==1&&s_settings.sound==3&&s_settings.vibration==2&&s_remaining==5940&&s_duration==5940);
 s_mode=Mode::Paused;s_remaining=1234;s_duration=5000;s_animationAt=987;
 app.initialScreen->onEnter();assert(s_dirty&&draws==1&&loads==4&&s_mode==Mode::Paused&&s_remaining==1234&&s_duration==5000&&s_animationAt==987);
 // Existing duplicate-activation guard and resume through view alone retain state.
 app.onEnter();app.initialScreen->onEnter();assert(loads==4&&s_remaining==1234&&s_mode==Mode::Paused);
 app.onExit();assert(!s_active&&s_mode==Mode::Setup&&saves==1&&stops==1&&previews==1);
 app.onEnter();assert(loads==8&&s_active&&s_remaining==5940);
 puts("PASS Pomodoro app-owned settings/activation, isolated view refresh and retained pause");
}
'''
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    for filename,preamble,names,screen,main in [
      ('wifi_setup_app.cpp',qr,['prepareWorkflow','tickAudioPreparation','enterView','retireView','exitApp','cover'],'kScreenWifiSetup',qr_main),
      ('pomodoro_app.cpp',pom,['redraw','setMode','prepareWorkflow','exitApp'],'kScreenPomodoro',pom_main)]:
        source=(ROOT/'src/apps'/filename).read_text()
        code=common+preamble+'\n'.join(function(source,n) for n in names)+'\n'
        code+=declaration(source,'static const ::scene::Screen '+screen)+'\n'
        code+=declaration(source,'static const ::scene::Screen* const kScreens')+'\n'
        if filename=='wifi_setup_app.cpp':
            start=source.index('static App makeApp()');end=source.index('\nApp app=makeApp();',start)
            code+=source[start:end]+'\nApp app=makeApp();\n'+main
        else:
            code+=declaration(source,'App app =')+'\n'+main
        path=root/filename;path.write_text(code);binary=root/(filename+'.test')
        subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)
