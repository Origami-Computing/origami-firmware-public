"""Compile real navigation and verify declared privileges with an unknown app."""
from pathlib import Path
import tempfile,subprocess,shutil
ROOT=Path(__file__).resolve().parents[1]
def main():
 with tempfile.TemporaryDirectory() as directory:
  temp=Path(directory)
  def write(name,text):
   p=temp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
  write('esp_timer.h','#pragma once\n#include <stdint.h>\nextern uint32_t clockMs;inline int64_t esp_timer_get_time(){return int64_t(clockMs)*1000;}')
  write('core/usage_events.h','#pragma once\n#include <stdint.h>\n#include <vector>\n#include <string>\nnamespace core{namespace usage{struct E{std::string app,event;int64_t duration;};inline std::vector<E> events;inline void emit(const char*a,const char*e,int64_t d=-1){events.push_back({a,e,d});}}}')
  write('ui/list_view.h','#pragma once\n#include <stdint.h>\nnamespace ui{inline void setRotaryScrollContext(uint32_t,bool){}}')
  write('ui/focused_label.h','#pragma once\n#include <stdint.h>\nnamespace ui{namespace focused_label{inline void reset(){} inline void restart(uint32_t){} template<class T>void beginFrame(T&,const void*,uint32_t){} template<class T>void finishFrame(T&){} }}')
  write('Arduino.h','#pragma once\n#include <stdint.h>\nextern uint32_t clockMs;inline uint32_t millis(){return clockMs;}\nstruct SerialStub{template<class... T> void printf(const char*,T...){} void println(const char*){}};extern SerialStub Serial;')
  write('TB_TFT_eSPI.h','''#pragma once
struct TFT_eSPI{bool direct=false;};
struct TFT_eSprite{bool clipped=false;int width(){return 280;}int height(){return 240;}void setViewport(int,int y,int,int,bool){clipped=y!=0;}void resetViewport(){clipped=false;}};
''')
  write('hal/hal_input.h','''#pragma once
namespace hal{namespace input{enum class EventType{PowerSleep,Chord_AB,Scroll,ButtonC_Tap,ButtonA_Tap,ButtonA_Press,ButtonB_Tap};struct InputEvent{EventType type;int delta;unsigned traceId;unsigned rotaryGesture;bool rotaryUndo;};inline void resetRotaryEncoderPins(){}inline void beginRotaryInterrupts(){}inline void resyncRotaryQuadrature(){}}}
''')
  write('hal/hal_io.h','#pragma once\nnamespace hal{namespace io{inline void setVibration(bool){}}}\n')
  write('services/haptic_playback.h',r'''#pragma once
#include "core/haptic_effect.h"
namespace services{namespace haptic_playback{
struct Status{bool playing=false;unsigned maxGapUs=0,transitions=0;};
static Status state;
inline bool start(const core::haptic::Effect&){state.playing=true;return true;}
inline bool startLoop(const core::haptic::Effect&e,unsigned=4000){return start(e);}
inline void renewLoop(){}
inline void stop(bool=false){state.playing=false;}
inline Status status(){return state;}
}}
''')
  write('core/store.h','#pragma once\n#include <cstddef>\nnamespace core{namespace store{inline bool getBlob(const char*,const char*,void*,size_t){return false;}inline bool putBlob(const char*,const char*,const void*,size_t){return false;}}}\n')
  write('ui/system_status_bar.h','''#pragma once
#include <stdint.h>
#include <TB_TFT_eSPI.h>
extern int bars,directBars,glyphBars;
namespace ui{struct SystemStatusBar{void invalidate(){}void service(uint32_t){}void setBackgroundColor(uint16_t){}void paint(TFT_eSPI&t){t.direct?++directBars:++bars;}void paintDirect(){++directBars;}void paintGlyphOverlay(TFT_eSPI&,uint16_t){++glyphBars;}};}
''')
  write('hal/hal_display.h','''#pragma once
#include <TB_TFT_eSPI.h>
namespace hal{namespace display{extern bool optimized;extern TFT_eSPI panel,direct;inline bool isOptimizedRender(){return optimized;}inline void exitOptimizedRender(){optimized=false;}inline void enterOptimizedRender(){optimized=true;}inline void markAllDirty(){}inline TFT_eSPI&tft(){return panel;}inline TFT_eSPI&directTft(){return direct;}inline void beginDirectDraw(){}inline void endDirectDraw(){}}}
''')
  write('sfx/audio_group.h','namespace sfx{struct AudioGroup{};inline AudioGroup*activeAudioGroup(){return nullptr;}inline void setActiveAudioGroup(AudioGroup*){}}')
  write('sfx/sfx_lib.h','namespace sfx{constexpr int kSfxOpen=0,kSfxBack=1;inline void play(int){}}')
  write('services/automation.h','extern bool alertCapture; namespace services{namespace automation{inline bool wantsInputCapture(){return alertCapture;}}}')
  write('services.h','extern int powerSleeps; namespace services{inline void doBatterySleep(){++powerSleeps;}}')
  write('config/ui_layout.h','constexpr int kSystemStatusBarH=38;')
  write('nav_handler.h',(ROOT/'include/nav_handler.h').read_text())
  write('main.cpp',r'''
#include "nav_handler.h"
#include "scene/screen.h"
#include "hal/hal_display.h"
#include <Arduino.h>
#include <cassert>
#include "services/vibration.h"
#include "core/usage_events.h"
#include <vector>
SerialStub Serial;uint32_t clockMs=1000;
int powerSleeps=0;bool alertCapture=false;
int bars=0,directBars=0,glyphBars=0,hints=0,draws=0;bool clipped=false;
namespace hal{namespace display{bool optimized=false;TFT_eSPI panel,direct;}}
void beginScreenChrome(const void*){}bool takeScreenChromeRepaint(){return false;}
uint16_t screenChromeColor(const scene::Screen*){return 0;}
void hint(TFT_eSprite&){++hints;}
void childHint(TFT_eSprite&){assert(hints==1);++hints;}
AppPresentation childOverlay(){return {false,false,AppPresentation::StatusBar::Default,childHint};}
void draw(TFT_eSprite&s){++draws;clipped=s.clipped;}
AppPresentation selected{};
AppPresentation policy(){return selected;}
AppPresentation suppressOverlays(){return {false,true,AppPresentation::StatusBar::Default,nullptr};}
core::health::RuntimePolicy selectedRuntime;
core::health::RuntimePolicy runtimePolicy(){return selectedRuntime;}
int covers=0;void cover(){++covers;}
int resumes=0; bool pendingResume=false;
void resume(){++resumes;pendingResume=true;}
int activations=0, exits=0, viewEntries=0, viewExits=0, lastLifecycle=0;
void activate(){++activations;lastLifecycle=3;}
void deactivate(){assert(lastLifecycle==1);++exits;lastLifecycle=0;}
void viewEnter(){if(pendingResume){assert(resumes>0);pendingResume=false;}assert(activations>exits);++viewEntries;lastLifecycle=2;}
void viewExit(){++viewExits;lastLifecycle=1;}
std::vector<int> shutdownEvents;
App* blockedPush=nullptr;
void shutdownOverlay(){shutdownEvents.push_back(1);}
void shutdownChildView(){shutdownEvents.push_back(2);}
void shutdownChild(){shutdownEvents.push_back(3);auto& n=NavHandler::instance();n.push(blockedPush);n.shutdown();n.reenterRoot();}
void shutdownRootView(){shutdownEvents.push_back(4);}
void shutdownRoot(){shutdownEvents.push_back(5);}
std::vector<int> sleepEvents;
void prepareSleep(){sleepEvents.push_back(1);}
void sleepChildViewExit(){sleepEvents.push_back(2);}
void sleepChildExit(){sleepEvents.push_back(3);}
void sleepRootResume(){sleepEvents.push_back(4);}
void sleepRootEnter(){sleepEvents.push_back(5);}
void sleepRootExit(){sleepEvents.push_back(6);}
void forbiddenSleep(){assert(false);}
std::vector<int> ticks;
void rootPreTick(uint32_t now){assert(now==777);ticks.push_back(1);}
void childPreTick(uint32_t now){assert(now==777);ticks.push_back(2);}
void rootTick(uint32_t){ticks.push_back(3);}
void childTick(uint32_t){ticks.push_back(4);}
void overlayTick(uint32_t){ticks.push_back(5);}
void clear(){bars=directBars=glyphBars=hints=draws=0;clipped=false;}
int main(){
 hal::display::direct.direct=true;
 const scene::Screen screen={"view",nullptr,nullptr,nullptr,nullptr,nullptr,draw,"view",nullptr};
 App app=SCENE_APP("Unknown app",&screen,nullptr);
 app.onEnter=activate;
 auto& nav=NavHandler::instance();TFT_eSprite canvas;nav.init(&app);
 assert(activations==1);
 assert(nav.open(&app)&&nav.depth()==1&&activations==1);
 assert(!nav.open(nullptr)&&nav.depth()==1);
 nav.draw(canvas);assert(draws==1&&clipped&&bars==1&&hints==0&&!canvas.clipped);
 // An app's name is no longer an implicit full-panel or status-bar privilege.
 clear();app.name="Boot";nav.draw(canvas);assert(clipped&&bars==1&&glyphBars==0);
 app.presentation=policy;
 selected={true,false,AppPresentation::StatusBar::Hidden,hint};
 clear();nav.draw(canvas);assert(draws==1&&!clipped&&bars==0&&hints==1);
 // Hiding a hint must not accidentally hide the independent status bar.
 selected={false,true,AppPresentation::StatusBar::Default,hint};
 clear();nav.draw(canvas);assert(clipped&&hints==0&&bars==1);
 selected={true,false,AppPresentation::StatusBar::GlyphOverlay};
 clear();nav.draw(canvas);assert(!clipped&&glyphBars==1&&bars==0);
 hal::display::optimized=true;
 selected={false,false,AppPresentation::StatusBar::DirectWhenOptimized};
 clear();nav.draw(canvas);assert(draws==0&&directBars==1&&bars==0);
 selected.statusBar=AppPresentation::StatusBar::Hidden;
 clear();nav.draw(canvas);assert(draws==0&&directBars==0&&bars==0);
 selected.statusBar=AppPresentation::StatusBar::Default;
 clear();nav.draw(canvas);assert(draws==0&&directBars==0&&bars==0);
 hal::display::optimized=false;
 clear();nav.draw(canvas);assert(draws==1&&bars==1);
 // Overlay providers are ordinary descriptors, retained while covered.
 App overlayChild=SCENE_APP("overlay child",&screen,nullptr);
 selected={false,false,AppPresentation::StatusBar::Default,hint};
 nav.push(&overlayChild);clear();nav.draw(canvas);assert(hints==1);
 overlayChild.presentation=childOverlay;clear();nav.draw(canvas);assert(hints==2);
 overlayChild.presentation=suppressOverlays;clear();nav.draw(canvas);assert(hints==0&&bars==1);
 overlayChild.presentation=nullptr;
 hal::display::optimized=true;clear();nav.draw(canvas);assert(hints==1&&draws==0);
 hal::display::optimized=false;
 nav.presentSystemScreen(&screen);clear();nav.draw(canvas);assert(hints==0);
 nav.dismissSystemScreen();clear();nav.draw(canvas);assert(hints==1);
 scene::Screen customChrome=screen;customChrome.ownsStatusBar=[](){return true;};
 alertCapture=true;nav.presentSystemScreen(&customChrome);clear();nav.draw(canvas);assert(bars==0&&hints==0);
 nav.dismissSystemScreen();nav.presentSystemScreen(&screen);clear();nav.draw(canvas);assert(bars==1);
 nav.dismissSystemScreen();alertCapture=false;
 nav.pop();selected.drawOverlay=nullptr;clear();nav.draw(canvas);assert(hints==0);
 // A covered app retains constraints. A new ordinary app cannot override them.
 auto runtime=nav.runtimePolicy();assert(runtime.allowStepSampling&&runtime.allowIdleReturn&&runtime.allowBatterySleep&&!runtime.reportActivity);
 app.runtimePolicy=runtimePolicy;
 selectedRuntime={false,false,true,false};
 App covered=SCENE_APP("Ordinary covering app",&screen,nullptr);
 nav.push(&covered);runtime=nav.runtimePolicy();
 assert(!runtime.allowStepSampling&&!runtime.allowIdleReturn&&runtime.allowBatterySleep&&!runtime.reportActivity);
 nav.pop();app.runtimePolicy=nullptr;
 covered.runtimePolicy=runtimePolicy;selectedRuntime=core::health::whileBusy(true);
 nav.push(&covered);runtime=nav.runtimePolicy();assert(runtime.allowStepSampling&&!runtime.allowIdleReturn&&!runtime.allowBatterySleep&&runtime.reportActivity);
 nav.pop();runtime=nav.runtimePolicy();assert(runtime.allowStepSampling&&runtime.allowIdleReturn&&runtime.allowBatterySleep&&!runtime.reportActivity);
 // Reading an Agent reply may allow return home while still blocking sleep.
 selectedRuntime={true,true,false,false};app.runtimePolicy=runtimePolicy;
 runtime=nav.runtimePolicy();assert(runtime.allowIdleReturn&&!runtime.allowBatterySleep&&!runtime.reportActivity);
 // Manual standby is independent of automatic sleep: busy ordinary apps and
 // reply-reading still permit C-hold unless they explicitly deny it.
 assert(runtime.allowPowerSleep);
 nav.dispatch({hal::input::EventType::PowerSleep});assert(powerSleeps==1);
 selectedRuntime=core::health::whileBusy(true);
 nav.dispatch({hal::input::EventType::PowerSleep});assert(powerSleeps==2);
 selectedRuntime.allowPowerSleep=false;covered.runtimePolicy=nullptr;nav.push(&covered);
 nav.dispatch({hal::input::EventType::PowerSleep});assert(powerSleeps==2);
 assert(!nav.runtimePolicy().allowPowerSleep); // Covered owner's denial persists.
 nav.pop();selectedRuntime=core::health::RuntimePolicy{};
 alertCapture=true;nav.dispatch({hal::input::EventType::PowerSleep});assert(powerSleeps==2);
 alertCapture=false;nav.dispatch({hal::input::EventType::PowerSleep});assert(powerSleeps==3);
 // A covered root's background constraints survive until explicit change.
 selectedRuntime.allowBackgroundUploads=false;selectedRuntime.allowAutomaticUpdate=false;
 nav.runtimePolicyChanged();
 auto background=core::health::backgroundPolicy();assert(!background.allowUploads&&!background.allowAutomaticUpdate);
 nav.push(&covered);background=core::health::backgroundPolicy();assert(!background.allowUploads&&!background.allowAutomaticUpdate);
 selectedRuntime.allowBackgroundUploads=true;selectedRuntime.allowAutomaticUpdate=true;
 nav.runtimePolicyChanged();background=core::health::backgroundPolicy();assert(background.allowUploads&&!background.allowAutomaticUpdate);
 nav.pop();background=core::health::backgroundPolicy();assert(background.allowUploads&&background.allowAutomaticUpdate);
 // App activation is distinct from view re-entry and overlay restoration.
 const scene::Screen view={"lifecycle",viewEnter,viewExit,nullptr,nullptr,nullptr,nullptr,"view",nullptr};
 const scene::Screen other={"other",viewEnter,viewExit,nullptr,nullptr,nullptr,nullptr,"other",nullptr};
 const scene::Screen* catalog[]={&view,&other};
 App life=SCENE_APP_CATALOG_LIFECYCLE("Lifecycle","life",&view,activate,deactivate,"test",catalog);
 life.onResume=resume;life.onCover=cover;
 int before=activations;
 clockMs=2000;nav.push(&life);assert(core::usage::events.back().event=="app.enter");const auto usageCount=core::usage::events.size();assert(activations==before+1&&viewEntries==1&&resumes==0);
 scene::switchTo(&life,&other);assert(core::usage::events.size()==usageCount);assert(activations==before+1&&viewEntries==2&&viewExits==1&&resumes==0);
 clockMs=2500;nav.presentSystemScreen(&screen);assert(core::usage::events.back().event=="app.cover"&&core::usage::events.back().duration==500);assert(covers==1);nav.dismissSystemScreen();assert(activations==before+1&&viewEntries==3&&resumes==1&&!pendingResume);
 nav.push(&covered);assert(covers==2);assert(nav.open(&life)&&nav.depth()==2);assert(activations==before+1&&viewEntries==4&&resumes==2&&!pendingResume);
 nav.pop();assert(exits==1&&viewExits==2); // view exit precedes app teardown
 nav.push(&life);assert(activations==before+2&&viewEntries==5&&resumes==2);
 nav.popToRoot();assert(exits==2&&viewExits==3);
 nav.replaceRoot(&life);assert(activations==before+3&&viewEntries==6&&resumes==2);
 nav.reenterRoot();assert(activations==before+3&&viewEntries==7&&viewExits==4&&resumes==3&&!pendingResume);
 nav.replaceRoot(&app);assert(exits==3&&viewExits==5&&activations==before+4);
 const scene::Screen rootView={"root",nullptr,shutdownRootView,nullptr,nullptr,nullptr,nullptr,"root",nullptr};
 const scene::Screen childView={"child",nullptr,shutdownChildView,nullptr,nullptr,nullptr,nullptr,"child",nullptr};
 const scene::Screen overlay={"overlay",nullptr,shutdownOverlay,nullptr,nullptr,nullptr,nullptr,"overlay",nullptr};
 App shutdownRootApp=SCENE_APP("root",&rootView,shutdownRoot);
 App shutdownChildApp=SCENE_APP("child",&childView,shutdownChild);
 shutdownRootApp.onResume=resume;shutdownChildApp.onResume=resume;blockedPush=&app;
 nav.replaceRoot(&shutdownRootApp);nav.push(&shutdownChildApp);nav.presentSystemScreen(&overlay);
 int oldResumes=resumes,oldActivations=activations;
 nav.shutdown();assert(nav.depth()==0&&!nav.top()&&!nav.root()&&!nav.systemScreenActive());
 assert(!core::health::backgroundPolicy().allowAutomaticUpdate);
 assert((shutdownEvents==std::vector<int>{1,2,3,4,5}));
 assert(resumes==oldResumes&&activations==oldActivations);
 nav.shutdown();nav.prepareForSleep();nav.push(&app);nav.presentSystemScreen(&screen);
 assert(nav.depth()==0&&!nav.systemScreenActive()&&shutdownEvents.size()==5);
 nav.init(&app);assert(nav.depth()==1&&activations==oldActivations+1);
 // Sleep retains the root, with workflow preparation before child teardown.
 const scene::Screen sleepRootView={"sleep-root",sleepRootEnter,sleepRootExit,nullptr,nullptr,nullptr,nullptr,"root",nullptr};
 const scene::Screen sleepChildView={"sleep-child",nullptr,sleepChildViewExit,nullptr,nullptr,nullptr,nullptr,"child",nullptr};
 App sleepRoot=SCENE_APP("sleep-root",&sleepRootView,forbiddenSleep);
 App sleepChild=SCENE_APP("sleep-child",&sleepChildView,sleepChildExit);
 sleepRoot.onPrepareSleep=prepareSleep;sleepRoot.onResume=sleepRootResume;
 sleepChild.onPrepareSleep=forbiddenSleep; // Removed apps use ordinary teardown.
 nav.replaceRoot(&sleepRoot);nav.push(&sleepChild);sleepEvents.clear();
 nav.prepareForSleep();
 assert((sleepEvents==std::vector<int>{1,2,3,4,5}));
 assert(nav.depth()==1&&nav.root()==&sleepRoot&&sleepChild.currentScreen==nullptr);
 sleepEvents.clear();nav.reenterRoot();
 assert((sleepEvents==std::vector<int>{6,4,5}));
 sleepEvents.clear();nav.prepareForSleep();
 assert((sleepEvents==std::vector<int>{1})); // Root-only: no premature view refresh.
 sleepRoot.onPrepareSleep=nullptr;sleepEvents.clear();nav.prepareForSleep();
 assert(sleepEvents.empty()&&nav.depth()==1); // Ordinary roots need no hook.
 // Arbitrary apps own pre-screen servicing; no Agent names or global queries.
 sleepRoot.onExit=nullptr;
 const scene::Screen tickRoot={"root",nullptr,nullptr,rootTick,nullptr,nullptr,nullptr,"root",nullptr};
 const scene::Screen tickChild={"child",nullptr,nullptr,childTick,nullptr,nullptr,nullptr,"child",nullptr};
 const scene::Screen tickOverlay={"overlay",nullptr,nullptr,overlayTick,nullptr,nullptr,nullptr,"overlay",nullptr};
 App rootHook=SCENE_APP("unknown root",&tickRoot,nullptr);
 App childHook=SCENE_APP("unknown child",&tickChild,nullptr);
 rootHook.onPreTick=rootPreTick;childHook.onPreTick=childPreTick;
 nav.replaceRoot(&rootHook);ticks.clear();nav.tick(777);assert((ticks==std::vector<int>{1,3}));
 nav.push(&childHook);ticks.clear();nav.tick(777);assert((ticks==std::vector<int>{1,2,4}));
 nav.presentSystemScreen(&tickOverlay);ticks.clear();nav.tick(777);assert((ticks==std::vector<int>{5}));
 nav.dismissSystemScreen();ticks.clear();nav.tick(777);assert((ticks==std::vector<int>{1,2,4}));
 nav.pop();ticks.clear();nav.tick(777);assert((ticks==std::vector<int>{1,3}));
 nav.setWorkflowPolicy({true,false,false,false});
 assert(!nav.runtimePolicy().allowIdleReturn&&!nav.runtimePolicy().allowBatterySleep);
 nav.push(&childHook);nav.popToRoot();assert(!nav.runtimePolicy().allowIdleReturn);
 nav.setWorkflowPolicy({});assert(nav.runtimePolicy().allowIdleReturn);
 nav.setWorkflowPolicy({true,false,false,false});
 nav.shutdown();ticks.clear();nav.tick(777);assert(ticks.empty());
 assert(nav.runtimePolicy().allowIdleReturn&&nav.runtimePolicy().allowBatterySleep);
 // Production navigator + screen transitions + vibration arbitration.
 using namespace services::vibration;
 const scene::Screen feedbackView={"feedback",nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,"feedback",nullptr};
 App feedbackRoot=SCENE_APP("root",&feedbackView,nullptr);
 App feedbackChild=SCENE_APP("child",&feedbackView,nullptr);
 nav.init(&feedbackRoot);clockMs+=1000;stopAll();
 auto count=effectStarts();nav.dispatch({hal::input::EventType::Scroll,1});
 assert(effectStarts()==count);
 nav.push(&feedbackChild);assert(lastCue()==Cue::Select);
 clockMs+=1000;count=effectStarts();nav.dispatch({hal::input::EventType::Chord_AB,0});
 assert(nav.depth()==1&&lastCue()==Cue::Back&&effectStarts()==count+1);
 scene::Screen nextView=feedbackView;
 count=effectStarts();scene::switchTo(&feedbackRoot,&nextView);
 assert(lastCue()==Cue::Select&&effectStarts()==count+1);
 scene::switchTo(&feedbackRoot,&feedbackView,scene::ScreenTransition::Back);
 assert(lastCue()==Cue::Back);
 count=effectStarts();scene::switchTo(&feedbackRoot,&nextView,scene::ScreenTransition::Silent);
 assert(effectStarts()==count);
 // The effect must already exist inside potentially slow lifecycle callbacks.
 feedbackRoot.onCover=[](){assert(lastCue()==Cue::Select&&effectPlaying());};
 feedbackChild.onEnter=[](){assert(lastCue()==Cue::Select&&effectPlaying());};
 beginInputFeedback(false);nav.push(&feedbackChild);endInputFeedback(Cue::Select);
 assert(effectStarts()==count+1&&lastCue()==Cue::Select);
 feedbackChild.onExit=[](){assert(lastCue()==Cue::Back&&effectPlaying());};
 feedbackRoot.onResume=[](){assert(lastCue()==Cue::Back&&effectPlaying());};
 count=effectStarts();beginInputFeedback(true);nav.pop();endInputFeedback(Cue::Back);
 assert(effectStarts()==count+1&&lastCue()==Cue::Back);
 nextView.onExit=[](){assert(lastCue()==Cue::Select&&effectPlaying());};
 feedbackRoot.currentScreen=&nextView;
 count=effectStarts();beginInputFeedback(false);
 scene::switchTo(&feedbackRoot,&feedbackView);endInputFeedback(Cue::Select);
 assert(effectStarts()==count+1&&lastCue()==Cue::Select);






}
''')
  subprocess.run([shutil.which('c++') or 'c++','-std=c++11','-I'+str(temp),'-I'+str(ROOT/'include'),str(ROOT/'src/nav_handler.cpp'),str(ROOT/'src/core/health_policy.cpp'),str(ROOT/'src/services/vibration.cpp'),str(ROOT/'src/core/alert_vibration.cpp'),str(ROOT/'src/scene/screen.cpp'),str(temp/'main.cpp'),'-o',str(temp/'check')],check=True)
  subprocess.run([str(temp/'check')],check=True)
 print('PASS presentation/runtime policies plus activation, screen switch, cover/uncover, overlay, pop and root lifecycle')
if __name__=='__main__':main()
