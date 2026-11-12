#include "apps/tutorial_app.h"
#include <esp_heap_caps.h>
#include <new>
#include "ui/onboarding_scenes.h"
#include "ui/native_frame.h"
#include "apps/app_store_app.h"
#include "widgets/storage.h"
#include "core/secrets.h"
#include "hal/hal_wifi.h"
#include "hal/hal_power.h"
#include "apps/agent_app.h"
#include "apps/onboarding_flow.h"
#include "apps/onboarding_controls.h"
#include "ui/onboarding_flow_view.h"
#include "services/uploads.h"
#include "services/agent_coordination.h"
#include "services/vibration.h"
#include "services/onboarding_progress.h"
#include "apps/menu_app.h"
#include "apps/camera_menu_app.h"
#include "services/photo_storage.h"
#include "hal/hal_sd.h"
#include "ui/onboarding_approved_view.h"
#include "apps/wifi_setup_app.h"
#include "ui/onboarding_connect_view.h"
#include "assets/onboarding_home_eyes.h"
#include "core/board_time.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "services.h"
#include "sfx/sfx_typewriter.h"
#include "services/battery_alerts.h"
#include "ui/onboarding_navigation_view.h"
#include "ui/scene.h"
#include "ui/onboarding_motion.h"
#include "ui/home_eyes.h"
#include <Arduino.h>

namespace apps { namespace tutorial {
static void compositeNavigationHint(uint16_t*);
static unsigned s_compositedHints=0;
namespace {
using onboarding::Input;
using onboarding::Flow;
Flow s_flow;
services::custom_apps::Catalog* s_apps=nullptr;uint32_t s_catalogJob=0;bool s_catalogRequested=false;
onboarding::Controls s_controls;
bool s_audioSelected=false;
unsigned s_backCount=0;uint32_t s_usbSuccessAt=0;
bool s_controlSuccessDrawn=false;
services::photos::Target s_photo;
// Keep the idle agent socket paused through capture/review/send. The upload
// service owns its own overlapping network lease; release only after receipt.
services::agent_coordination::TransportPause s_photoTransport;
bool s_photoQueued=false,s_reviewCached=false;
char s_photoError[80]={};
enum class PhotoStop { None, Review, Failure, Retake };
PhotoStop s_photoStop=PhotoStop::None;
// Remove only this photo's queue reference after the worker releases it. A busy
// transfer keeps ownership; never delete or resubmit its source in the meantime.
bool releasePhotoQueue(){
 if(!s_photoQueued)return true;
 auto* q=services::uploads::readyQueue();if(!q)return false;
 UploadQueue::FileMutation mutation(*q,s_photo.path,true);
 if(!mutation.ready())return false;
 s_photoQueued=false;return true;
}
void describePhotoError(const char* reason){
 const char* text=reason;
 if(strstr(reason,"401")||strstr(reason,"403")||strstr(reason,"credentials"))text="Sign in again on your phone, then retry.";
 else if(strstr(reason,"Timeout")||strstr(reason,"receipt"))text="The server did not confirm delivery. Try again.";
 else if(strstr(reason,"Connect")||strstr(reason,"Wi-Fi"))text="Wi-Fi disconnected. Reconnect and try again.";
 snprintf(s_photoError,sizeof(s_photoError),"%s",text);
}
std::string s_wrapped;
onboarding::TextPlayback s_text;
bool s_talkPending=false,s_agentStarted=false;
bool s_active=false,s_bootChecked=false,s_dirty=true,s_completed=false;
uint32_t s_started=0,s_lastDraw=0,s_drawUs=0;
uint32_t s_exitHoldAt=0;
bool s_exitHolding=false;
constexpr unsigned kScanReadingPauseMs=2200;
unsigned s_scanAfter=0;
int s_scrollFrom=0,s_scrollTarget=0;
uint32_t s_scrollAt=0;
int websiteOffset(){
 const float t=std::min(1.f,float(millis()-s_scrollAt)/160.f);
 return s_scrollFrom+int((s_scrollTarget-s_scrollFrom)*ui::onboarding_intro::ease(t));
}
unsigned s_speechLetters=0,s_speechBlips=0;
bool s_speechOpen=false;
void stopSpeech(){if(s_speechOpen)sfx::typewriterTeardown();s_speechOpen=false;}
TFT_eSprite s_dialogueCache(&hal::display::directTft());
TFT_eSprite s_navHint(&hal::display::directTft());
ui::OnboardingMotion s_motion;
bool s_motionReady=false;
int s_hintStyle=-1;
int s_cachePage=-1,s_cacheLetters=-1,s_cacheScroll=-1;
void releaseDialogue(){s_dialogueCache.deleteSprite();s_cachePage=s_cacheLetters=-1;}
void prepareMotion(){
 s_motion.close();s_motionReady=false;
 // Open-menu animation is owned by the shared approved scene painter.
 if(s_flow.page==34){
  s_motionReady=s_motion.prepareOutro();s_dialogueCache.setColorDepth(16);
  if(!s_dialogueCache.created())s_dialogueCache.createSprite(280,240);
  if(s_dialogueCache.created()){
   const auto wrapped=ui::onboarding_approved::wrap(ui::onboarding_approved::copy(22));
   const unsigned n=ui::onboarding_intro::lineCount(wrapped.c_str());
   ui::onboarding_approved::proposal(s_dialogueCache,22,120000,n>3?n-3:0,88,120,ui::onboarding_approved::copy(22),false,true);
  }else s_motionReady=false;
 }
 if(s_flow.page==18||s_flow.page==33||s_flow.page==34)Serial.printf("[tutorial-motion] page=%u ready=%u\n",s_flow.page,s_motionReady);
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool s_frozen=false;
bool s_preview=false;unsigned s_previewSelection=0;char s_previewDetail[128]={};
unsigned s_frozenMs=0;
#endif
unsigned elapsed(){
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 if(s_frozen)return s_frozenMs;
#endif
 return millis()-s_started;
}
const char* dialogueCopy(){return s_wrapped.c_str();}
void refreshCopy(){s_wrapped=ui::onboarding_approved::wrap(ui::onboarding_approved::copy(s_flow.page));}
unsigned textRows(){return s_flow.page==24?2:s_flow.page==25?3:ui::onboarding_approved::textRows(120);}
unsigned readyAt(){return 0;}
unsigned legacyReadyAt(){if(s_flow.page==8||s_flow.page==14||s_flow.page==30||s_flow.page==31||s_flow.page==34)return 0;return 450+unsigned(s_wrapped.size())*ui::onboarding_intro::kCharacterMs+140;}
void serviceSpeech(unsigned ms){
 static unsigned phase=0;
 if(s_flow.page!=124){stopSpeech();phase=0;s_speechLetters=0;return;}
 const auto speech=onboarding::DemoSpeech::at(ms%11000);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 if(s_frozen){stopSpeech();phase=speech.phase;s_speechLetters=speech.count;return;}
#endif
 if(phase!=speech.phase||speech.count<s_speechLetters){stopSpeech();s_speechLetters=0;phase=speech.phase;}
 if(speech.count>s_speechLetters){
  unsigned at=speech.count-1;while(at>s_speechLetters&&speech.text[at]<=' ')--at;
  if(sfx::typewriterServiceChar(speech.text[at],s_speechBlips))++s_speechBlips;
  s_speechOpen=sfx::typewriterIsOpen();s_speechLetters=speech.count;
 }else if(!speech.phase||speech.count==strlen(speech.text))stopSpeech();
}

void save(){
 if(s_flow.wifiOnly)return;
 if(!services::onboarding_progress::save(s_flow.page,s_completed,s_flow.interest,s_flow.account,s_flow.joinWifi,s_flow.setupPublished,s_flow.connected)) Serial.println("[tutorial] progress save failed");
}
bool load(){
 const auto p=services::onboarding_progress::load();s_flow.reset();s_flow.interest=p.interest;s_flow.account=p.account;s_flow.joinWifi=p.joinWifi;s_flow.setupPublished=p.setupPublished;s_flow.connected=p.connected||hal::wifi::connected();s_flow.origin=p.account?40:(p.joinWifi?41:39);s_flow.setPage(p.page);s_completed=p.completed;return !s_completed;
}
void enterView(){
 s_controls.reset(s_flow.page);s_controlSuccessDrawn=false;s_usbSuccessAt=0;s_catalogRequested=false;
 refreshCopy();s_reviewCached=false;
 stopSpeech();s_speechLetters=s_speechBlips=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;s_preview=false;
#endif
 releaseDialogue();prepareMotion();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_scanAfter=readyAt()+kScanReadingPauseMs;
 s_started=millis();s_lastDraw=0;s_dirty=true;
}
void changed(){
 s_controls.reset(s_flow.page);s_controlSuccessDrawn=false;s_usbSuccessAt=0;s_catalogRequested=false;
 refreshCopy();s_reviewCached=false;
 stopSpeech();s_speechLetters=s_speechBlips=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;s_preview=false;
#endif
 releaseDialogue();prepareMotion();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_scanAfter=readyAt()+kScanReadingPauseMs;s_started=millis();s_lastDraw=0;s_dirty=true;save();}
void prepareWorkflow(){
 ui::setNativeFrameOverlay(compositeNavigationHint);
 if(!s_active){s_active=true;load();s_flow.reset();}
 else if(s_flow.page==12){if(s_flow.interest||s_flow.setupPublished)s_flow.account=core::secrets::backendConfig().apiKey[0]!=0;s_flow.wifiReturned(apps::wifi_setup::takeTutorialResult());}
 else if(s_flow.page==19){s_flow.connected=s_flow.connected||hal::wifi::connected();s_flow.menuReturned();}
 else if(s_flow.page==129){auto result=apps::app_store::takeTutorialResult();s_flow.installed|=result==apps::app_store::TutorialResult::Installed;s_flow.setPage(result==apps::app_store::TutorialResult::Back?128:133);}
 else if(s_flow.page==30){auto photo=apps::camera_menu::takeTutorialPhoto();if(photo.valid()){s_photo=photo;s_photoQueued=false;s_flow.page=31;Serial.println("[tutorial-photo] review camera_released=1 reconnect_requested=1 queued=0");}else{s_photoTransport.release();s_flow.page=40;}}
 refreshCopy();
 save();
}
void exitApp(){ui::setNativeFrameOverlay(nullptr);if(s_catalogJob)widgets::storage::abandon(s_catalogJob);s_catalogJob=0;if(s_apps){s_apps->~Catalog();heap_caps_free(s_apps);s_apps=nullptr;}s_photoTransport.release();stopSpeech();s_active=false;releaseDialogue();s_motion.close();s_navHint.deleteSprite();s_hintStyle=-1;if(s_photo.valid()&&!s_photoQueued)hal::sd::removeFile(s_photo.path);s_photo={};s_photoQueued=false;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;s_preview=false;
#endif
}
void startScanner(){
 s_flow.page=12;changed();apps::wifi_setup::beginTutorial();
}
void finish();
void tick(uint32_t now){
 // C click edges are delivered on release by the power-button gesture owner.
 // Read the existing physical level for visual feedback without changing that policy.
 if(s_flow.page==5&&!s_controls.success){
  const bool down=hal::input::buttonCDown();
  if(down!=s_controls.held){
   const unsigned feedback=s_controls.button(2,down,now);
   if(feedback)services::vibration::navigationFeedback(services::vibration::Cue::Scroll);
   s_dirty=true;
  }
 }
 if(s_flow.wifiOnly && s_flow.page==Flow::kFinished){NavHandler::instance().pop();return;}
 if(now-s_lastDraw>=40)s_dirty=true;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 if(s_frozen)return;
#endif
 if(!s_controls.success)s_controlSuccessDrawn=false;
 if(onboarding::Controls::accepts(s_flow.page)&&s_controlSuccessDrawn&&s_controls.complete(now)){s_flow.setPage(s_controls.next());changed();return;}
 if(s_flow.page==Flow::kBuildConnect){
  if(!s_usbSuccessAt&&int32_t(now-s_started)>=1800&&hal::power::externalPowerPresent(hal::power::read())){
   stopSpeech();services::battery_alerts::acknowledgeExternalPower();s_usbSuccessAt=millis();s_dirty=true;
  }
  if(s_usbSuccessAt&&int32_t(now-s_usbSuccessAt)>=1200){s_flow.setPage(Flow::kBuildBrowse);changed();return;}
 }
 if(s_flow.page==129){apps::app_store::beginTutorial();return;}
 if(s_flow.page==133||s_flow.page==134){
  if(!s_apps&&!s_catalogRequested){
   void* memory=heap_caps_malloc(sizeof(services::custom_apps::Catalog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
   if(memory)s_apps=new(memory) services::custom_apps::Catalog{};else s_catalogRequested=true;
  }
  if(s_apps&&!s_catalogRequested&&!s_catalogJob){s_catalogJob=widgets::storage::request(widgets::storage::Operation::Catalog);s_catalogRequested=s_catalogJob!=0;}
  services::custom_apps::Result result;
  if(s_catalogJob&&widgets::storage::take(s_catalogJob,result,nullptr,s_apps)){s_catalogJob=0;s_dirty=true;}
 }
 if(s_flow.page==34){if(elapsed()>=6500){s_flow.outroFinished();finish();}return;}
 if(s_flow.page==30){if(s_photoTransport.pollAcquire())apps::camera_menu::beginTutorialCapture();return;}
 if(s_flow.page==Flow::kSending){
  auto* queue=services::uploads::readyQueue();
  const auto delivery=queue?queue->delivery(s_photo.path):UploadQueue::Delivery{};
  if(delivery.confirmed){Serial.println("[tutorial-photo] server receipt confirmed");s_photoTransport.release();s_photoStop=PhotoStop::None;s_flow.photoDelivered();s_photo={};s_photoQueued=false;changed();return;}
  if(s_photoStop==PhotoStop::None){
   const char* reason=delivery.error[0]?delivery.error:!hal::wifi::connected()&&elapsed()>15000?"Wi-Fi disconnected":elapsed()>90000?"Timeout":nullptr;
   if(reason){describePhotoError(reason);s_photoStop=PhotoStop::Failure;}
  }
  if(s_photoStop!=PhotoStop::None&&releasePhotoQueue()){
   const auto action=s_photoStop;s_photoStop=PhotoStop::None;
   s_flow.setPage(action==PhotoStop::Failure?Flow::kPhotoError:31);changed();
  }
  return;
 }
 // Speech is serviced after painting, synchronized to the visible text.
 // QR scan begins on explicit B after the instructions, as approved.

}
void finish(){s_completed=true;save();NavHandler::instance().popToRoot();}
void onInput(hal::input::InputEvent ev){
 using ET=hal::input::EventType;Input input;
 if(onboarding::Controls::accepts(s_flow.page)){
  unsigned haptics=0;
  switch(ev.type){
   case ET::ButtonA_Press:haptics=s_controls.button(0,true,millis());break;
   case ET::ButtonA_Release:haptics=s_controls.button(0,false,millis());break;
   case ET::ButtonB_Press:haptics=s_controls.button(1,true,millis());break;
   case ET::ButtonB_Release:haptics=s_controls.button(1,false,millis());break;
   case ET::ButtonC_Press:haptics=s_controls.button(2,true,millis());break;
   case ET::ButtonC_Release:haptics=s_controls.button(2,false,millis());break;
   case ET::Scroll:haptics=s_controls.turn(-ev.delta,millis(),ev.rotaryUndo);break;
   default:break;
  }
  if(haptics)services::vibration::navigationFeedback(services::vibration::Cue::Scroll);
  else services::vibration::suppressInputFeedback();
  s_dirty=true;return;
 }
 switch(ev.type){
  case ET::ButtonA_Tap:input=Input::A;break;
  case ET::ButtonB_Tap:input=Input::B;break;
  case ET::ButtonC_Tap:input=Input::C;break;
  case ET::Scroll:if(!ev.delta)return;input=Input::Rotate;break;
  default:return;
 }
 if(s_flow.page==Flow::kSending){if(input==Input::A)s_photoStop=PhotoStop::Review;return;}
 if(input==Input::Rotate){
  if(s_flow.turn(ev.delta)){services::vibration::navigationFeedback(services::vibration::Cue::Select);changed();
   if(s_flow.page==19){s_backCount=0;NavHandler::instance().push(&apps::menu::app);}
   return;
  }
  if(s_flow.page==22){s_text.turn(ev.delta,ui::onboarding_intro::lineCount(dialogueCopy()),textRows(),0);s_dirty=true;}return;
 }
 if(s_flow.page==31||s_flow.page==Flow::kPhotoError){
  const bool retry=s_flow.page==31?input==Input::B:input==Input::C&&s_flow.selection==0;
  const bool retake=input==Input::A||(s_flow.page==Flow::kPhotoError&&input==Input::C&&s_flow.selection==1);
  if(retry){
   if(!s_photo.valid()){describePhotoError("Photo unavailable. Retake it.");s_flow.setPage(Flow::kPhotoError);changed();return;}
   auto& q=services::uploads::queue();q.watchDelivery(s_photo.path);
   if(!services::photos::enqueue(s_photo)){describePhotoError("Upload queue unavailable. Try again.");s_flow.setPage(Flow::kPhotoError);changed();return;}
   s_photoQueued=true;s_photoError[0]=0;s_photoStop=PhotoStop::None;
   s_flow.setPage(Flow::kSending);changed();return;
  }
  if(retake){
   if(!releasePhotoQueue())return;
   if(s_photo.valid())hal::sd::removeFile(s_photo.path);
   s_photo={};s_flow.setPage(30);changed();return;
  }
 }
 if(s_flow.page==Flow::kLogin&&input==Input::B&&(s_flow.interest||s_flow.setupPublished))s_flow.account=core::secrets::backendConfig().apiKey[0]!=0;
 if(!s_flow.input(input)){services::vibration::suppressInputFeedback();return;}
 services::vibration::navigationFeedback(services::vibration::Cue::Select);changed();
 if(s_flow.wifiOnly&&s_flow.page==Flow::kFinished){NavHandler::instance().pop();return;}
 if(s_flow.page==12){releaseDialogue();apps::wifi_setup::beginTutorial(true);}
 else if(s_flow.page==23)finish();
}

bool onChord(){
 if(s_flow.wifiOnly){NavHandler::instance().pop();return true;}
 if(s_flow.page==123){s_flow.setPage(124);changed();return true;}
 if(elapsed()+s_text.offset<readyAt()||!s_flow.beginTalk())return true;
 stopSpeech();save();releaseDialogue();
 auto& nav=NavHandler::instance();nav.popToRoot();
 // Hand the same physical chord to the normal home agent, preserving release-to-send.
 s_active=true;s_talkPending=true;s_agentStarted=false;
 nav.setWorkflowPolicy({true,false,false,false,false});
 hal::input::InputEvent chord;chord.type=hal::input::EventType::Chord_AB;nav.dispatch(chord);s_agentStarted=!apps::agent::isAtHomescreen();
 return true;
}
void clock(TFT_eSprite& c){char time[8]={0};core::time::formatClockHHMM(time,sizeof(time));ui::pixel_text::centered(c,time[0]?time:"--:--",15,2);}
void drawHome(TFT_eSprite& c,unsigned ms){
 using namespace ui::onboarding_intro;
 c.fillSprite(kUiBg);
 auto& chrome=NavHandler::instance().chrome();chrome.service(millis());chrome.paintGlyphOverlay(hal::display::tft(),kUiBg);
 struct Shift {TFT_eSprite& c;int dy;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y+dy,w,h,ink);}};
 if(s_flow.page==25)ui::onboarding_baked::talkEyes(c,ms);
 else ui::onboarding_baked::eyes(c,ms);
 if(s_flow.page==18){onboarding_navigation::pointingHand(c,ms);onboarding_navigation::instruction(c,false,true);}
 else {
  c.fillRect(200,8,80,30,kUiBg);
  if(s_flow.page==25)c.fillRect(20,8,38,30,kUiBg);
  const unsigned textMs=std::min(ms,onboarding_navigation::talkReady(s_flow.page));
  const int letters=textMs<450?0:(textMs-450)/kCharacterMs;
  const int top=s_flow.page==25?128:164;
  if(!s_dialogueCache.created()){s_dialogueCache.setColorDepth(16);s_dialogueCache.createSprite(280,240-top);}
  if(s_dialogueCache.created()){
   if(s_cachePage!=int(s_flow.page)||s_cacheLetters!=letters||s_cacheScroll!=int(s_text.scroll)){
    s_dialogueCache.fillSprite(kUiBg);
    Shift layer{s_dialogueCache,-top};onboarding_navigation::talkInstruction(layer,s_flow.page,textMs,s_text.scroll);
    s_cachePage=s_flow.page;s_cacheLetters=letters;s_cacheScroll=s_text.scroll;
   }
   s_dialogueCache.pushToSprite(&c,0,top);
   onboarding_wifi::arrows(c,s_flow.page==25,s_flow.page!=25||ms<onboarding_navigation::talkReady(s_flow.page));
  }else onboarding_navigation::talkInstruction(c,s_flow.page,ms,s_text.scroll);
 }

}
void buildScene(ui::scene::Scene& sc,int16_t,int16_t){
 sc.clear();const char* copy=dialogueCopy();
 if(s_flow.page==9){
  for(int i=0;i<2;++i){const bool selected=i==int(s_audioSelected);
   sc.addLabel(i?"tutorial.method.audio":"tutorial.method.qr",{int16_t(i?154:34),206,70,18},i?"Audio":"QR Code",selected?0xffff:0,selected?0:0xffff,1);
  }
 }
 sc.addLabel("tutorial.dialogue",{38,88,214,94},copy,kUiFg,kUiBg,2);
}
void draw(TFT_eSprite& c){
 if(!s_dirty)return;s_dirty=false;s_lastDraw=millis();const uint32_t started=micros();
 const unsigned ms=elapsed();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 if(s_preview){ui::onboarding_scenes::render(c,s_flow.page,ms,s_previewSelection,s_previewDetail);s_drawUs=micros()-started;hal::display::markAllDirty();return;}
#endif
 if(s_flow.page==31){
  if(!s_reviewCached){
   c.fillSprite(kUiBg);
   const bool rendered=s_photo.valid()&&hal::display::drawJpegFileScaled(s_photo.path,0,0,280,240,hal::display::ImageFit::Cover);
   ui::onboarding_approved::photoReviewChrome(c);
   if(!rendered)ui::design::instruction(c,"Photo unavailable");
   s_reviewCached=true;
  }
 }else{
  unsigned selection=s_flow.selection;char detail[80]={};
  if(onboarding::Controls::accepts(s_flow.page))snprintf(detail,sizeof(detail),"%.1f,%u,%u",s_controls.angleDegrees(),s_controls.held,s_controls.success);
  if(s_flow.page==Flow::kBuildConnect)selection=s_usbSuccessAt?1:0;
  if(s_flow.page>=40&&s_flow.page<=42||s_flow.page==128)selection=ORIGAMI_STAGING_BUILD?0:1;
  if(s_flow.page==Flow::kPhotoError)snprintf(detail,sizeof(detail),"%s",s_photoError);
  if(s_flow.page==Flow::kPhotoSent)snprintf(detail,sizeof(detail),"Available in your account");
  if(s_flow.page==132)snprintf(detail,sizeof(detail),"%s",apps::app_store::lastInstalledName());
  if(s_flow.page==133||s_flow.page==134)selection=s_flow.installed?1:0;
  if(s_flow.page==133||s_flow.page==134){
   const char* names[services::custom_apps::kCatalogEntries];
   const unsigned count=s_apps?s_apps->count:0;
   for(unsigned i=0;i<count;++i)names[i]=s_apps->entries[i].name;
   ui::onboarding_scenes::customMenu(c,s_flow.installed,s_flow.page==134,ms,names,count);
  }else ui::onboarding_scenes::render(c,s_flow.page,ms,selection,detail);
 }
 serviceSpeech(ms);
 s_drawUs=micros()-started;hal::display::markAllDirty();
 // Start the dwell after painting, so queued input or a busy UI loop cannot skip green.
 if(s_controls.success&&!s_controlSuccessDrawn){s_controlSuccessDrawn=true;s_controls.successAt=millis();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  Serial.printf("[tutorial-feedback] page=%u greenAt=%lu\n",s_flow.page,(unsigned long)s_controls.successAt);
#endif
 }
}
const ::scene::Screen kScreen={"Meet Kami",enterView,nullptr,tick,onInput,onChord,draw,"learn",buildScene};
const ::scene::Screen* const kScreens[]={&kScreen};
} // namespace
static void drawNavigationHint(TFT_eSprite&);
static AppPresentation presentationPolicy() {
    const bool active=isActive();
    return {active, active, active ? AppPresentation::StatusBar::Hidden : AppPresentation::StatusBar::Default, drawNavigationHint};
}
static core::health::RuntimePolicy runtimePolicy() {
    const bool active=sessionActive();
    return {true, !active, !active, false, !active};
}
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Meet Kami","tutorial",&kScreen,prepareWorkflow,prepareWorkflow,exitApp,"src/apps/tutorial_app.cpp",kScreens, presentationPolicy, runtimePolicy);
bool isActive(){return s_active&&NavHandler::instance().top()==&app;}
bool sessionActive(){return s_active;}
bool navigationLessonActive(){return s_active&&s_flow.page==19;}
void noteNavigationBack(){if(navigationLessonActive()){++s_backCount;s_hintStyle=-1;}}
bool navigationPracticed(){return navigationLessonActive()&&s_flow.practiced;}
void noteNavigation(){if(navigationLessonActive())s_flow.practiced=true;}
bool returnFromNavigation(){
 if(!navigationLessonActive())return false;
 if(!s_flow.practiced)return true;
 noteNavigationBack();
 auto& nav=NavHandler::instance();
 if(nav.top()==&apps::menu::app)return false; // Menu owns its own category history.
 if(nav.depth()>1&&nav.top()!=&app)nav.pop();
 return true;
}
static int prepareNavigationHint(){
 if(!navigationLessonActive())return -1;
 const int style=s_flow.practiced?(s_backCount?2:1):0,top=style?164:194,h=240-top;
 if(!s_navHint.created()||s_hintStyle!=style){
  s_navHint.deleteSprite();s_navHint.setColorDepth(16);s_navHint.createSprite(280,h);
  if(!s_navHint.created())return -1;
  struct Offset{TFT_eSprite& c;int top;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y-top,w,h,ink);}} offset{s_navHint,top};
  s_navHint.fillSprite(kUiBg);if(style==2)ui::onboarding_scenes::backPrompt(offset);else ui::onboarding_approved::menuInstruction(offset,s_flow.practiced);s_hintStyle=style;
 }
 return top;
}
static void compositeNavigationHint(uint16_t* pixels){
 auto& nav=NavHandler::instance();
 if(nav.systemScreenActive()||appPresentation(nav.top()).hideAppOverlays)return;
 const int top=prepareNavigationHint();if(top<0)return;
 ++s_compositedHints;
 for(int x=0;x<280;++x)for(int y=top;y<240;++y){
  const uint16_t color=s_navHint.readPixel(x,y-top);
  pixels[(279-x)*240+y]=(color>>8)|(color<<8);
 }
}
static void drawNavigationHint(TFT_eSprite& c){
 const int top=prepareNavigationHint();if(top<0)return;const int h=240-top;
 if(hal::display::isOptimizedRender()){hal::display::beginDirectDraw();s_navHint.pushSprite(0,top);hal::display::endDirectDraw();}
 else{s_navHint.pushToSprite(&c,0,top);hal::display::markDirty(top,h);}
}

void replay(){
 s_audioSelected=false;
 NavHandler::instance().setWorkflowPolicy({});
 s_talkPending=s_agentStarted=false;
 auto& nav=NavHandler::instance();nav.popToRoot();load();s_flow.reset();s_completed=false;s_active=true;s_bootChecked=true;
 services::notifyUserActivity();nav.push(&app);
}
void beginWifiSetup(){
 s_audioSelected=false;
 s_talkPending=s_agentStarted=false;
 s_flow.beginWifiSetup();s_active=true;
 services::notifyUserActivity();NavHandler::instance().push(&app);
}
void serviceBootOffer(){
 auto& nav=NavHandler::instance();
 // Service the whole workflow, including its scanner, menu and Agent children.
 if(s_active && !s_flow.wifiOnly && hal::input::buttonADown() && hal::input::buttonBDown()) {
  if(!s_exitHolding){s_exitHolding=true;s_exitHoldAt=millis();}
  if(uint32_t(millis()-s_exitHoldAt)>=5000u){
   s_exitHolding=false;s_talkPending=s_agentStarted=false;
   stopSpeech();save();nav.setWorkflowPolicy({});nav.popToRoot();s_active=false;
   hal::input::suppressUntilButtonsReleased();return;
  }
 } else s_exitHolding=false;
 if(s_talkPending){
  if(!apps::agent::isAtHomescreen())s_agentStarted=true;
  if(s_agentStarted&&nav.depth()==1&&!nav.systemScreenActive()&&apps::agent::isAtHomescreen()){
   s_talkPending=false;s_flow.agentReturned();nav.push(&app);nav.setWorkflowPolicy({});
  }
  return;
 }
 if(s_bootChecked||nav.depth()!=1||nav.systemScreenActive())return;
 s_bootChecked=true;
 const auto progress=services::onboarding_progress::load();
 load();
 if(!progress.completed){
  if(!progress.offered && !services::onboarding_progress::markOffered()){Serial.println("[tutorial] first-boot marker save failed");return;}
  s_active=true;services::notifyUserActivity();nav.push(&app);
 }
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool restartForTesting(){
 if(!services::onboarding_progress::resetForDevelopment())return false;
 NavHandler::instance().setWorkflowPolicy({});
 s_talkPending=s_agentStarted=false;NavHandler::instance().popToRoot();s_bootChecked=false;return true;
}
void printStatus(){Serial.printf("[tutorial] compositedHints=%u\n",s_compositedHints);Serial.printf("[tutorial-route] interest=%u account=%u wifi=%u selection=%u\n",s_flow.interest,s_flow.account,s_flow.joinWifi,s_flow.selection);const auto audio=sfx::typewriterStats();Serial.printf("[tutorial] page=%u elapsed=%u ready=%u active=%u practiced=%u completed=%u reveal=%u scroll=%u agent=%u recording=%u thinking=%u reply=%u error=%u drawUs=%lu fps=%.1f saved=%08lx speechOpen=%u speechBlips=%u audioBlips=%lu audioAvgGap=%lu audioMaxGap=%lu motion=%u\n",s_flow.page,elapsed(),readyAt(),isActive(),s_flow.practiced,s_completed,s_text.offset,s_text.scroll,s_talkPending,apps::agent::isRecording(),apps::agent::isSending(),apps::agent::isOnReplyScreen(),apps::agent::isShowingError(),(unsigned long)s_drawUs,hal::display::currentFps(),(unsigned long)services::onboarding_progress::savedRecordForDevelopment(),s_speechOpen,s_speechBlips,(unsigned long)audio.played,(unsigned long)(audio.played>1?audio.totalGapMs/(audio.played-1):0),(unsigned long)audio.maxGapMs,s_motionReady);}
bool debugSeek(unsigned page,unsigned ms){
 if(!isActive()||page>143||(page>=12&&page<=15)||page==19||page==20||page==23||page==27||page==30||page==31)return false;
 s_preview=false;stopSpeech();s_speechLetters=s_speechBlips=0;releaseDialogue();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_flow.setPage(page==26?25:page);s_controls.reset(s_flow.page);s_controlSuccessDrawn=false;refreshCopy();s_scanAfter=readyAt()+kScanReadingPauseMs;s_text.scroll=0;s_frozen=true;s_frozenMs=ms;prepareMotion();s_dirty=true;return true;
}
bool debugPreview(unsigned page,unsigned ms,unsigned selection,const char* detail){
 if(ms>120000||selection>8||!debugSeek(page,ms))return false;
 s_preview=true;s_previewSelection=selection;snprintf(s_previewDetail,sizeof(s_previewDetail),"%s",detail?detail:"");
 for(char* at=s_previewDetail;*at;++at)if(at[0]=='\\'&&at[1]=='n'){*at='\n';memmove(at+1,at+2,strlen(at+2)+1);}
 s_dirty=true;return true;
}
void debugPlay(){s_started=millis()-elapsed();s_frozen=false;s_preview=false;s_dirty=true;}
#endif
}} // namespace apps::tutorial
