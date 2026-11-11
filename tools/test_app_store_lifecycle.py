"""Exercise the production QR app workflow against controlled camera/worker boundaries."""
from pathlib import Path
import subprocess,tempfile,os,shlex
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 for name in ['apps/app_store_app.h','widgets/storage.h','services/wifi_qr_session.h','services/qr_decode.h','services/background_network.h','hal/hal_display.h','hal/hal_input.h','hal/hal_wifi.h','widgets/sync.h','widgets/app.h','nav_handler.h','scene/screen.h','ui/qr_preview.h','ui/scene.h','ui/onboarding_scenes.h','ui/nav_controls.h','Arduino.h']:
  f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('#pragma once\n')
 (p/'test.cpp').write_text(r'''
#include "app.h"
#include "core/app_store_qr.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
namespace services {namespace background_network {struct PeriodicWork {static inline bool wanted=false;static void prioritizeInstall(bool v){wanted=v;}};}}
unsigned clockMs=0;unsigned millis(){return clockMs;}
class TFT_eSprite{public:void fillRect(int,int,int,int,int){}};
namespace hal {
namespace input {enum class EventType{Scroll,Enter,Back,ButtonA_Tap,ButtonB_Tap,ButtonC_Tap};struct InputEvent{EventType type;int delta=0;};void configureRotaryEncoderPins(){}}
namespace camera {struct CameraFrame{const uint8_t* buf=nullptr;size_t len=0;};}
namespace display {TFT_eSprite& canvas(){static TFT_eSprite c;return c;}void flush(){}int bypass=0;void enterOptimizedRender(){++bypass;}void exitOptimizedRender(){--bypass;}void markAllDirty(){}void fixColorInversion(){}}
namespace wifi {bool online=false;bool connected(){return online;}}
}

namespace apps {namespace app_store {extern App app;enum class TutorialResult{None,Back,Skipped,Installed};}}
namespace ui {
namespace design {void center(TFT_eSprite&,const char* text,int,int){assert(!strcmp(text,"Loading"));}}
namespace onboarding_scenes {void scannerLoading(TFT_eSprite&,unsigned){}template<class C>void render(C&,unsigned,unsigned,unsigned,const char*){}}
struct Rect {int x,y,w,h;};
namespace scene {enum class TextAlign{TopCenter};struct Node{int font=1;};struct Scene{Node node;int bar=-1;std::string body;bool activity=false;void clear(){bar=-1;body.clear();activity=false;}void addRect(const char* id,ui::Rect r,int,int=0,int z=0){if(!strcmp(id,"store.activity")){activity=true;assert(z>2);assert(r.x>=24&&r.x+r.w<=256);}}Node* addLabel(const char* id,ui::Rect,const char* text,int,int,int,TextAlign){if(!strcmp(id,"store.body"))body=text;return &node;}void addProgressBar(const char*,ui::Rect r,int value,int maximum,int,int,int,int){assert(r.x==24&&r.w==232&&maximum==100);bar=value;}void render(TFT_eSprite&,int,int){}};Scene& scratchScene(){static Scene s;return s;}}
struct QrPreview{void setAppLesson(bool){}void close(){}bool render(const uint8_t*,size_t){return true;}};
namespace nav {bool isBackTap(hal::input::InputEvent e){return e.type==hal::input::EventType::Back;}bool isEnterTap(hal::input::InputEvent e){return e.type==hal::input::EventType::Enter;}}
}
namespace scene {struct Screen{const char* name;void(*enter)();void(*exit)();void(*tick)(uint32_t);void(*input)(hal::input::InputEvent);bool(*chord)();void(*draw)(TFT_eSprite&);const char* id;void(*build)(ui::scene::Scene&,int16_t,int16_t);const uint16_t* background;};}
struct NavHandler {static NavHandler& instance(){static NavHandler n;return n;}void runtimePolicyChanged(){}void pop(){}void push(App*){}};
namespace services {
namespace custom_apps {enum class Medium{Flash,Sd};enum class Result{Ok,Missing,Unavailable,Corrupt,Invalid,Incompatible,NoMemory,NoSpace,Io,Cancelled,Uncertain,Busy,Network};const char* resultName(Result){return "IO error";}}
namespace qr {std::string decoded;bool pollPayload(char* out,size_t n){if(decoded.empty())return false;snprintf(out,n,"%s",decoded.c_str());decoded.clear();return true;}}
namespace wifi_qr {
struct Session {
 enum class Decoder{Waiting,Ready,Failed,RadioFailed};enum class Frame{Idle,DecoderStopped,Missing,Rendered,Submitted};
 bool beginOk=true,cameraOk=true,released=true;int closes=0,stops=0;Decoder decoder=Decoder::Ready;
 static Session& instance(){static Session s;return s;}
 enum class Startup{Pending,Ready,Busy}; Startup admission=Startup::Ready; Startup pollBegin(){return admission;} void cancelPendingBegin(){}Decoder prepareDecoder(void*){return decoder;}enum class Exposure{SettleBeforePreview,SettleDuringPreview};bool openCamera(Exposure e){assert(e==Exposure::SettleDuringPreview);return cameraOk;}
 void stopCapture(){++stops;}void fail(){++stops;}bool releaseStopped(){return released;}
 bool close(void(&retire)()){++closes;retire();return released;}Frame renderFrame(bool(&)(const hal::camera::CameraFrame&)){return Frame::Rendered;}
};}
}
namespace widgets {namespace storage {
struct DownloadProgress {enum class Phase{Preparing,Downloading,Installing};Phase phase=Phase::Preparing;int percent=-1;};
DownloadProgress reported;DownloadProgress downloadProgress(uint32_t){return reported;}

unsigned queued=0,abandoned=0;bool complete=false,busy=false;services::custom_apps::Result result=services::custom_apps::Result::Ok;
uint32_t requestDownload(const char*){if(busy)return 0;return ++queued;}
bool take(uint32_t,services::custom_apps::Result& out){if(!complete)return false;out=result;complete=false;return true;}
void abandon(uint32_t id){abandoned=id;}
}}
'''+'\n#include "'+str(ROOT/'src/apps/app_store_app.cpp')+'"\n'+r'''
int main(){
 using namespace apps::app_store;using ET=hal::input::EventType;
 ui::scene::Scene rendered;step=Step::Install;
 progress={widgets::storage::DownloadProgress::Phase::Downloading,50};build(rendered,280,202);
 assert(rendered.bar==50&&rendered.body=="Downloading... 50%"&&!rendered.activity);
 progress={};for(progressFrame=0;progressFrame<94;++progressFrame){build(rendered,280,202);assert(rendered.activity&&rendered.bar==0);}
 progress={widgets::storage::DownloadProgress::Phase::Installing,100};build(rendered,280,202);
 assert(rendered.bar==100&&rendered.body=="Checking and saving...");
 step=Step::Done;build(rendered,280,202);assert(rendered.bar==-1);
 step=Step::Install;job=99;widgets::storage::complete=true;
 widgets::storage::result=services::custom_apps::Result::Network;tick(0);
 assert(step==Step::Error&&!strcmp(message,"Cannot reach store.\nCheck Wi-Fi, then retry."));
 widgets::storage::result=services::custom_apps::Result::Ok;
 app.onEnter();assert(step==Step::Choose);input({ET::Enter});tick(0);assert(ownsScanner&&step==Step::Prepare);
 tick(1);assert(step==Step::Scan&&hal::display::bypass==1);
 services::qr::decoded="origami-app:https://apps.example.com/a.kapp#sha256="+std::string(64,'a');
 tick(2);assert(step==Step::Release&&hal::display::bypass==0);
 scanner.released=false;tick(3);assert(step==Step::Release&&widgets::storage::queued==0);
 scanner.released=true;tick(4);assert(step==Step::Confirm&&!ownsScanner);
 input({ET::Enter});tick(5);assert(step==Step::WaitNetwork&&widgets::storage::queued==0);
 hal::wifi::online=true;tick(6);assert(step==Step::Install&&widgets::storage::queued==1);
 assert(services::background_network::PeriodicWork::wanted);assert(policy().allowBackgroundUploads); // Must not deadlock its own worker admission.
 widgets::storage::result=services::custom_apps::Result::Network;
 widgets::storage::complete=true;tick(7);assert(step==Step::Error&&job==0&&!services::background_network::PeriodicWork::wanted);
 input({ET::Enter});assert(step==Step::WaitNetwork&&!ownsScanner);tick(8);
 assert(step==Step::Install&&widgets::storage::queued==2); // No second camera scan.
 widgets::storage::result=services::custom_apps::Result::Ok;
 widgets::storage::complete=true;tick(9);assert(step==Step::Done&&job==0);
 app.onExit();assert(widgets::storage::abandoned==0);
 app.onEnter();input({ET::Enter});tick(10);tick(11);assert(ownsScanner);
 app.onCover();assert(step==Step::Error&&!ownsScanner&&hal::display::bypass==0&&scanner.closes==1);
 app.onExit();assert(scanner.closes==1);
 app.onEnter();input({ET::Enter});scanner.cameraOk=false;tick(20);tick(21);
 assert(step==Step::Error&&hal::display::bypass==0);tick(22);assert(!ownsScanner);
 scanner.cameraOk=true;app.onExit();app.onEnter();input({ET::Enter});tick(30);tick(31);
 services::qr::decoded="not an app QR";tick(32);assert(step==Step::Error&&widgets::storage::queued==2);app.onExit();
 job=42;step=Step::Install;app.onExit();assert(job==0&&widgets::storage::abandoned==42);
 requestTutorial=true;app.onEnter();assert(step==Step::Start&&tutorial);
 input({ET::ButtonA_Tap});assert(tutorialResult==TutorialResult::Back);app.onExit();
 requestTutorial=true;app.onEnter();input({ET::ButtonB_Tap});assert(tutorialResult==TutorialResult::Skipped);app.onExit();
 requestTutorial=true;app.onEnter();step=Step::Done;input({ET::ButtonB_Tap});assert(tutorialResult==TutorialResult::Installed);app.onExit();
 puts("PASS QR workflow: release before network, confirmation, Wi-Fi recovery, cover, startup failure, invalid QR and cancellation");
}
''')
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-I'+str(p),'-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
