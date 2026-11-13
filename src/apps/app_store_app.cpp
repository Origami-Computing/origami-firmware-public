#include "apps/app_store_app.h"
#include "ui/onboarding_scenes.h"
#include "widgets/storage.h"
#include "core/app_store_qr.h"
#include "services/wifi_qr_session.h"
#include "services/qr_decode.h"
#include "services/background_network.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_wifi.h"
#include "widgets/sync.h"
#include "widgets/app.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/qr_preview.h"
#include "ui/scene.h"
#include "ui/nav_controls.h"
#include <Arduino.h>
#include <cstring>
namespace apps { namespace app_store {
namespace {
enum class Step { Choose, Start, Prepare, Scan, Release, Confirm, WaitNetwork, Install, Done, Error };
Step step=Step::Choose;
bool tutorial=false,requestTutorial=false;unsigned tutorialSelection=0;TutorialResult tutorialResult=TutorialResult::None;
uint32_t started=0,loadingAt=0,lastLoadingPaint=0;
bool presented=false;
void paintLoading(bool force=false) {
 const uint32_t now=millis();
 if(!force && now-lastLoadingPaint<80)return;
 lastLoadingPaint=now;
 ui::onboarding_scenes::scannerLoading(hal::display::canvas(),now-loadingAt);
 hal::display::markAllDirty();hal::display::flush();
}
char payload[256]{},message[64]{},installedName[49]{};
bool bypass=false,ownsScanner=false,retryDownload=false;
uint32_t job=0,progressFrame=0;
widgets::storage::DownloadProgress progress;
ui::QrPreview preview;
auto& scanner=services::wifi_qr::Session::instance();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Service input substitutes only for optical decoding; the foreground installer
// still releases its camera and uses the normal network and storage paths.
char serviceCode[7]{};
#endif
void setStep(Step next){if(next==Step::Start){started=loadingAt=millis();presented=false;paintLoading(true);}services::background_network::PeriodicWork::prioritizeInstall(next==Step::Release||next==Step::WaitNetwork||next==Step::Install);if(next==Step::Install)progress={};step=next;NavHandler::instance().runtimePolicyChanged();hal::display::markAllDirty();}
void retire(){preview.close();if(bypass)hal::display::exitOptimizedRender();bypass=false;hal::display::markAllDirty();}
void error(const char* text){snprintf(message,sizeof(message),"%s",text);if(ownsScanner)scanner.fail();retire();setStep(Step::Error);}
void enter(){tutorial=requestTutorial;requestTutorial=false;preview.setAppLesson(tutorial);tutorialSelection=0;retryDownload=false;setStep(tutorial?Step::Start:Step::Choose);started=millis();payload[0]=message[0]=0;}
void closeScanner(){if(step==Step::Start)scanner.cancelPendingBegin();if(ownsScanner){(void)scanner.close(retire);ownsScanner=false;}else retire();}
void exit(){
 services::background_network::PeriodicWork::prioritizeInstall(false);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 serviceCode[0]=0;
#endif
 if(job)widgets::storage::abandon(job);job=0;closeScanner();payload[0]=0;}
void cover(){if(ownsScanner||step==Step::Start){closeScanner();snprintf(message,sizeof(message),"Scanner interrupted.\nRetry to scan.");setStep(Step::Error);}}
bool frame(const hal::camera::CameraFrame& f){
 const bool grayReady=preview.render(f.buf,f.len);
 // The first decoder submission follows a successful JPEG presentation.
 if(grayReady && !presented){
  presented=true;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  Serial.printf("[app-store] first preview after %lu ms\n",(unsigned long)(millis()-loadingAt));
#endif
 }
 return grayReady;
}
void tick(uint32_t){
 // Input may start a new step after the navigation loop sampled its clock.
 const uint32_t now=millis();
 if(tutorial && now-lastLoadingPaint>=40 && (step==Step::Install||step==Step::WaitNetwork||step==Step::Release||step==Step::Done)){lastLoadingPaint=now;hal::display::markAllDirty();}
 if(step==Step::Error){if(ownsScanner&&scanner.releaseStopped())ownsScanner=false;return;}
 if(step==Step::Start){
  paintLoading();
  auto admission=scanner.pollBegin();
  if(admission==services::wifi_qr::Session::Startup::Pending){
   if(millis()-started>=10000){scanner.cancelPendingBegin();error("Scanner startup timed out");}
   return;
  }
  if(admission!=services::wifi_qr::Session::Startup::Ready){error("Scanner busy. Try again");return;}
  ownsScanner=true;setStep(Step::Prepare);started=now;return;
 }
 if(step==Step::Prepare){
  paintLoading();
  auto d=scanner.prepareDecoder(nullptr);
  if(d==services::wifi_qr::Session::Decoder::Waiting){if(now-started>10000)error("Scanner startup timed out");return;}
  if(d!=services::wifi_qr::Session::Decoder::Ready){error("Scanner unavailable");return;}
  if(!scanner.openCamera(services::wifi_qr::Session::Exposure::SettleDuringPreview)){error("Camera unavailable");return;}
  hal::display::enterOptimizedRender();bypass=true;
  hal::display::fixColorInversion();hal::input::configureRotaryEncoderPins();
  setStep(Step::Scan);started=millis();return;
 }
 if(step==Step::Scan){
  bool decodedPayload=services::qr::pollPayload(payload,sizeof(payload));
#ifdef ORIGAMI_DEV_SERIAL_INPUT
   if(!decodedPayload && serviceCode[0]){strcpy(payload,serviceCode);decodedPayload=true;}
   serviceCode[0]=0;
#endif
   if(decodedPayload){
   core::app_store::Download decoded;
   if(!core::app_store::parse(payload,decoded)){error("Scan a Kami app-store QR");return;}
   scanner.stopCapture();retire();setStep(Step::Release);started=now;return;
  }
  auto f=scanner.renderFrame(frame);
  if(f==services::wifi_qr::Session::Frame::DecoderStopped){error("Scanner stopped. Try again");return;}
  if(!presented && now-started>8000){error("Camera supplied no preview");return;}
  if(now-started>60000)error("No QR read. Try again");
  return;
 }
 if(step==Step::Release){
  if(!scanner.releaseStopped())return;
  ownsScanner=false;setStep(tutorial?Step::WaitNetwork:Step::Confirm);started=now;return;
 }
 if(step==Step::WaitNetwork){
  if(!hal::wifi::connected()){
   if(now-started>30000)error("Wi-Fi unavailable.\nConnect, then retry.");
   return;
  }
  job=widgets::storage::requestDownload(payload);
  if(!job){error("Storage busy or QR invalid.\nTry again.");return;}
  setStep(Step::Install);started=now;return;
 }
 if(step==Step::Install){
  auto next=widgets::storage::downloadProgress(job);
  if(next.phase!=progress.phase||next.percent!=progress.percent||(next.percent<0&&now/160!=progressFrame)){
   progress=next;progressFrame=now/160;hal::display::markAllDirty();
  }
  services::custom_apps::Result result;
  services::custom_apps::Package package;
  if(widgets::storage::take(job,result,&package)){
   job=0;
   if(result==services::custom_apps::Result::Ok){
    services::custom_apps::View view;
    installedName[0]=0;
    if(services::custom_apps::inspect(package.bytes,package.size,view)==services::custom_apps::Result::Ok)snprintf(installedName,sizeof(installedName),"%s",view.name);
    setStep(Step::Done);
   }
   else {
    using Result=services::custom_apps::Result;
    switch(result){
     case Result::Network:error("Cannot reach store.\nCheck Wi-Fi, then retry.");break;
     case Result::NoSpace:error("Storage full.\nDelete an app first.");break;
     case Result::Unavailable:error("Storage unavailable.\nCheck the SD card.");break;
     case Result::Incompatible:error("Update firmware\nto use this app.");break;
     case Result::Corrupt:case Result::Invalid:error("App check failed.\nScan a fresh QR.");break;
     case Result::Missing:error("App not found.\nScan a fresh QR.");break;
     case Result::NoMemory:case Result::Busy:error("Device busy.\nTry again shortly.");break;
     case Result::Uncertain:error("Check installed apps\nbefore retrying.");break;
     case Result::Cancelled:error("Installation cancelled");break;
     default:error("Could not save app.\nTry again.");break;
    }
   }
  }else if(now-started>120000){
   widgets::storage::abandon(job);job=0;error("Download timed out.\nCheck installed apps.");
  }
 }
}
void input(hal::input::InputEvent ev){
 if(tutorial){
  if(step==Step::Install||step==Step::WaitNetwork||step==Step::Release)return;
  using E=hal::input::EventType;
  if(ev.type==E::ButtonA_Tap){
   if(step==Step::Done)return;
   if(step==Step::Error){if(!ownsScanner){retryDownload=false;setStep(Step::Start);started=millis();}return;}
   tutorialResult=TutorialResult::Back;NavHandler::instance().pop();return;
  }
  if(ev.type==E::ButtonB_Tap){if(step==Step::Error)return;tutorialResult=step==Step::Done?TutorialResult::Installed:TutorialResult::Skipped;NavHandler::instance().pop();return;}
  if(step==Step::Error){
   if(ev.type==E::Scroll){tutorialSelection=ev.delta>0?1:0;hal::display::markAllDirty();}
   if(ev.type==E::ButtonC_Tap){if(tutorialSelection){tutorialResult=TutorialResult::Skipped;NavHandler::instance().pop();}else if(!ownsScanner){retryDownload=false;setStep(Step::Start);started=millis();}}
  }
  return;
 }

 if(ui::nav::isBackTap(ev)){NavHandler::instance().pop();return;}
 if(ui::nav::isEnterTap(ev)){
  if(step==Step::Choose){setStep(Step::Start);return;}
  if(step==Step::Error){if(!ownsScanner){if(retryDownload){setStep(Step::WaitNetwork);started=millis();}else enter();}return;}
  if(step==Step::Confirm){
   retryDownload=true;setStep(Step::WaitNetwork);started=millis();return;
  }
  if(step==Step::Done)NavHandler::instance().pop();
 }
}
// One stable 4 px grid and two existing font roles across every install state.
// White page, ink text and hairline rules; only a failure is tinted.
constexpr uint16_t kPaper=0xFFFF, kInk=0x1926, kRule=0xDEFB, kRose=0xFB2C, kTrack=0xDEFB;
void label(ui::scene::Scene& s,const char* id,ui::Rect bounds,const char* text,uint16_t background,bool title=false){
 auto* node=s.addLabel(id,bounds,text,kInk,background,title?2:1,ui::scene::TextAlign::TopCenter);
 if(node)node->font=title?1:2;
}
void build(ui::scene::Scene& s,int16_t,int16_t){
 s.clear();s.addRect("store.bg",{0,0,280,202},kPaper);
 const bool failed=step==Step::Error;
 const uint16_t rule=failed?kRose:kRule;
 const char* title=step==Step::Done?"App installed":failed?"Let's try again":"Install an app";
 label(s,"store.title",{16,24,248,24},title,kPaper,true);
 s.addRect("store.heading",{24,60,232,1},rule);
 const char* body=step==Step::Choose?"Pick an app in the store.\nScan its QR to install.":step==Step::Confirm?"App QR found.\nReady to install?":step==Step::Done?"Ready in Custom Apps.\nYour other apps are unchanged.":failed?message:step==Step::Install?"Downloading app...":(step==Step::Release||step==Step::WaitNetwork)?"Connecting to Wi-Fi...":"Starting the camera...";
 char progressText[48];
 if(step==Step::Install){
  using Phase=widgets::storage::DownloadProgress::Phase;
  if(progress.phase==Phase::Installing)body="Checking and saving...";
  else if(progress.phase==Phase::Preparing)body="Preparing download...";
  else if(progress.percent>=0){snprintf(progressText,sizeof(progressText),"Downloading... %d%%",progress.percent);body=progressText;}
  s.addProgressBar("store.progress",{24,132,232,12},progress.percent<0?0:progress.percent,100,kTrack,kInk,4,2);
  if(progress.percent<0)s.addRect("store.activity",{int16_t(24+(progressFrame%47)*4),132,48,12},kInk,4,3);
 }
 // Scene labels are single-line; keep bounded lines instead of relying on wrapping.
 char lines[96];snprintf(lines,sizeof(lines),"%s",body);
 char* second=strchr(lines,'\n');if(second)*second++=0;
 label(s,"store.body",{16,76,248,20},lines,kPaper);
 if(second)label(s,"store.body2",{16,100,248,20},second,kPaper);
 if(step==Step::Choose)label(s,"store.connection",{16,132,248,20},hal::wifi::connected()?"No account needed":"Wi-Fi needed to download",kPaper);
 s.addRect("store.footer",{24,164,232,1},rule);
 label(s,"store.hint",{16,172,248,20},step==Step::Choose?"C: scan QR   A: back":step==Step::Confirm?"C: install   A: cancel":step==Step::Done?"C: return to apps":failed?(retryDownload?"C: retry   A: cancel":"C: scan again   A: back"):"A: cancel",kPaper);
}
void draw(TFT_eSprite& canvas){if(bypass)return;
 if(step==Step::Start||step==Step::Prepare){ui::onboarding_scenes::scannerLoading(canvas,millis()-loadingAt);return;}
 if(tutorial){
  unsigned page=step==Step::Done?132:step==Step::Error?131:step==Step::Install||step==Step::WaitNetwork||step==Step::Release?130:129;
  ui::onboarding_scenes::render(canvas,page,millis()-started,tutorialSelection,step==Step::Error?message:step==Step::Done?(installedName[0]?installedName:"Ready in Custom Apps"):"Please wait");
  hal::display::markAllDirty();return;
 }
auto& s=ui::scene::scratchScene();build(s,280,202);s.render(canvas,0,38);hal::display::markAllDirty();}
const ::scene::Screen screen={"Install app",nullptr,nullptr,tick,input,nullptr,draw,"scan",build,&kPaper};
const ::scene::Screen* const screens[]={&screen};
core::health::RuntimePolicy policy(){
 // Foreground downloads still use the widget worker's normal upload admission.
 auto p=core::health::whileBusy(step!=Step::Done&&step!=Step::Error&&step!=Step::Choose);
 p.allowAutomaticUpdate=false;return p;
}
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool submitStoreCode(const char* code){
 if(!ownsScanner||step!=Step::Scan||serviceCode[0]||!code||strlen(code)!=6||strspn(code,"0123456789")!=6)return false;
 memcpy(serviceCode,code,7);return true;
}
const char* installDiagnostic(){return step==Step::Done?"installed":step==Step::Error?message:step==Step::Scan?"scanning":"working";}
#endif
void beginTutorial(){requestTutorial=true;tutorialResult=TutorialResult::None;NavHandler::instance().push(&app);}
TutorialResult takeTutorialResult(){auto result=tutorialResult;tutorialResult=TutorialResult::None;return result;}
const char* lastInstalledName(){return installedName[0]?installedName:"Ready in Custom Apps";}
App app=[] { App descriptor=SCENE_APP_CATALOG_LIFECYCLE_POLICIES("Install app","app-store",&screen,enter,exit,"src/apps/app_store_app.cpp",screens,nullptr,policy);
 descriptor.onCover=cover;
 // The scanner loading view paints and flushes the whole panel from tick, so the
 // status bar must not be composited over it, or the two alternate every frame.
 // The tutorial owns the whole panel too.
 descriptor.presentation=[](){
  const bool fullPanel=tutorial||step==Step::Start||step==Step::Prepare;
  return AppPresentation{fullPanel,fullPanel,fullPanel?AppPresentation::StatusBar::Hidden:AppPresentation::StatusBar::Default};};
 return descriptor;}();
} }
