#include "apps/tutorial_app.h"
#include "apps/agent_app.h"
#include "apps/onboarding_flow.h"
#include "services/onboarding_progress.h"
#include "apps/menu_app.h"
#include "apps/camera_menu_app.h"
#include "services/photo_storage.h"
#include "hal/hal_sd.h"
#include "ui/onboarding_approved_view.h"
#include "apps/wifi_setup_app.h"
#include "assets/onboarding_home_eyes.h"
#include "core/board_time.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "services.h"
#include "sfx/sfx_typewriter.h"
#include "ui/onboarding_navigation_view.h"
#include "ui/scene.h"
#include "ui/onboarding_motion.h"
#include "ui/home_eyes.h"
#include <Arduino.h>

namespace apps { namespace tutorial {
namespace {
using onboarding::Input;
using onboarding::Flow;
Flow s_flow;
services::photos::Target s_photo;
bool s_photoQueued=false,s_reviewCached=false;
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
 if(s_flow.page==18)s_motionReady=s_motion.prepareWheel();
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
unsigned readyAt(){if(s_flow.page==8||s_flow.page==14||s_flow.page==30||s_flow.page==31||s_flow.page==34)return 0;return 450+unsigned(s_wrapped.size())*ui::onboarding_intro::kCharacterMs+140;}
void serviceSpeech(unsigned ms){
 if(s_flow.page==8||s_flow.page==14||(s_flow.page>=30&&s_flow.page!=32&&s_flow.page!=33)){stopSpeech();return;}
 const char* copy=dialogueCopy();
 const unsigned starts=450;
 const unsigned count=ms<starts?0:std::min(unsigned(strlen(copy)),(ms-starts)/ui::onboarding_intro::kCharacterMs);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 if(s_frozen){stopSpeech();s_speechLetters=count;return;}
#endif
 if(s_text.offset||!copy[0]){stopSpeech();s_speechLetters=count;return;}
 if(count>s_speechLetters){
  // Match agent replies: play the latest revealed character, never a backlog
  // of blips after a slow frame. Scrolling already-visible text stays silent.
  unsigned at=count-1;while(at>s_speechLetters&&copy[at]<=' ')--at;
  const char ch=copy[at];
  if(ch>' '){
   // Use one repeating note sequence on every speaking page. Advancing only
   // after an actual blip keeps whitespace, wrapping and missed frames from
   // turning the same SFX into a different page-specific melody.
   if(sfx::typewriterServiceChar(ch,s_speechBlips))++s_speechBlips;
   s_speechOpen=sfx::typewriterIsOpen();
  }
  s_speechLetters=count;
 }else if(count==strlen(copy))stopSpeech();
}
void save(){
 if(s_flow.wifiOnly)return;
 if(!services::onboarding_progress::save(s_flow.page,s_completed)) Serial.println("[tutorial] progress save failed");
}
bool load(){
 const auto p=services::onboarding_progress::load();s_flow.reset();s_flow.page=p.page;s_completed=p.completed;return !s_completed;
}
void enterView(){
 refreshCopy();s_reviewCached=false;
 stopSpeech();s_speechLetters=s_speechBlips=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;
#endif
 releaseDialogue();prepareMotion();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_scanAfter=readyAt()+kScanReadingPauseMs;
 s_started=millis();s_lastDraw=0;s_dirty=true;
}
void changed(){
 refreshCopy();s_reviewCached=false;
 stopSpeech();s_speechLetters=s_speechBlips=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;
#endif
 releaseDialogue();prepareMotion();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_scanAfter=readyAt()+kScanReadingPauseMs;s_started=millis();s_lastDraw=0;s_dirty=true;save();}
void prepareWorkflow(){
 if(!s_active){s_active=true;load();s_flow.reset();}
 else if(s_flow.page==12)s_flow.wifiReturned(apps::wifi_setup::takeTutorialResult());
 else if(s_flow.page==19)s_flow.menuReturned();
 else if(s_flow.page==30){auto photo=apps::camera_menu::takeTutorialPhoto();if(photo.valid()){s_photo=photo;s_photoQueued=false;s_flow.page=31;Serial.println("[tutorial-photo] review camera_released=1 reconnect_requested=1 queued=0");}else s_flow.page=29;}
 refreshCopy();
 save();
}
void exitApp(){stopSpeech();s_active=false;releaseDialogue();s_motion.close();s_navHint.deleteSprite();s_hintStyle=-1;if(s_photo.valid()&&!s_photoQueued)hal::sd::removeFile(s_photo.path);s_photo={};s_photoQueued=false;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;
#endif
}
void startScanner(){
 s_flow.page=12;changed();apps::wifi_setup::beginTutorial();
}
void finish();
void tick(uint32_t now){
 if(s_flow.wifiOnly && s_flow.page==Flow::kFinished){NavHandler::instance().pop();return;}
 if(now-s_lastDraw>=33)s_dirty=true;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 if(s_frozen)return;
#endif
 if(s_flow.page==34){if(elapsed()>=6500){s_flow.outroFinished();finish();}return;}
 serviceSpeech(elapsed()+s_text.offset);
 // QR scan begins on explicit B after the instructions, as approved.

}
void finish(){s_completed=true;save();NavHandler::instance().popToRoot();}
void onInput(hal::input::InputEvent ev){
 using ET=hal::input::EventType;Input input;
 switch(ev.type){
  case ET::ButtonA_Tap:input=Input::A;break;
  case ET::ButtonB_Tap:input=Input::B;break;
  case ET::ButtonC_Tap:input=Input::C;break;
  case ET::Scroll:if(!ev.delta)return;input=Input::Rotate;break;
  default:return;
 }
 if(s_flow.page==31){
  if(input==Input::B&&!s_photoQueued){
   if(!services::photos::enqueue(s_photo)){ui::design::instruction(hal::display::tft(),"Queue full. B retries");hal::display::markAllDirty();return;}
   s_photoQueued=true;Serial.printf("[tutorial-photo] send sequence=%lu queued=1\n",(unsigned long)s_photo.sequence);
  }else if(input==Input::A){
   if(s_photo.valid()&&!s_photoQueued)hal::sd::removeFile(s_photo.path);
   s_photo={};s_photoQueued=false;
  }
 }
 const unsigned age=elapsed(),starts=450;
 if(input==Input::B&&s_text.reveal(age,starts,readyAt())){stopSpeech();s_dirty=true;return;}
 if(input==Input::Rotate&&s_flow.page!=18){
  if(age+s_text.offset<readyAt())return; // Follow the newest line until typing finishes.
  const char* copy=dialogueCopy();
  const unsigned count=(age+s_text.offset-starts)/ui::onboarding_intro::kCharacterMs;
  const int oldOffset=websiteOffset();
  if(s_text.turn(ev.delta,s_flow.page==8?textRows()+onboarding_wifi::kWebsiteScrollMax:ui::onboarding_intro::lineCount(copy),textRows(),s_flow.page==8?0:s_text.follow(copy,count,textRows()))){
   if(s_flow.page==8){s_scrollFrom=oldOffset;s_scrollTarget=onboarding_wifi::websiteShift(s_text.scroll);s_scrollAt=millis();}if(s_flow.page==9)s_scanAfter=std::max(s_scanAfter,age+s_text.offset+kScanReadingPauseMs);s_dirty=true;return;}
 }
 if(!s_flow.input(input,s_flow.page==18||age+s_text.offset>=readyAt()))return;
 changed();
 if(s_flow.wifiOnly && s_flow.page==Flow::kFinished){NavHandler::instance().pop();return;}
 if(s_flow.page==12){releaseDialogue();apps::wifi_setup::beginTutorial();}
 else if(s_flow.page==30)apps::camera_menu::beginTutorialCapture();
 else if(s_flow.page==19)NavHandler::instance().push(&apps::menu::app);
 else if(s_flow.page==23)finish();
}
bool onChord(){
 if(s_flow.wifiOnly){NavHandler::instance().pop();return true;}
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
 sc.addLabel("tutorial.dialogue",{38,88,214,94},copy,kUiFg,kUiBg,2);
}
void draw(TFT_eSprite& c){
 if(!s_dirty)return;s_dirty=false;s_lastDraw=millis();const uint32_t started=micros();
 const unsigned ms=elapsed()+s_text.offset;
 if(s_flow.page==31){
  if(!s_reviewCached){
   c.fillSprite(kUiBg);
   const bool rendered=s_photo.valid()&&hal::display::drawJpegFileScaled(s_photo.path,0,0,280,240,hal::display::ImageFit::Cover);
   ui::onboarding_approved::photoReviewChrome(c);
   if(!rendered)ui::design::instruction(c,"Photo unavailable");
   s_reviewCached=true;
  }
 }else if(s_flow.page==34){
  if(!s_motionReady||!s_motion.drawOutro(c,s_dialogueCache,ms,NavHandler::instance().chrome())){c.fillSprite(kUiBg);ui::design::center(c,"Goodbye!",108);}
 }else if(s_flow.page==8)ui::onboarding_approved::setupQr(c,ms,"Scan me",true,true);
 else if(s_flow.page==18&&s_motionReady){
  if(!s_dialogueCache.created()){s_dialogueCache.setColorDepth(16);s_dialogueCache.createSprite(280,240);}
  if(s_dialogueCache.created()){
   if(s_cachePage!=18){ui::onboarding_approved::navigation(s_dialogueCache,18,0,0);s_cachePage=18;}
   s_dialogueCache.pushToSprite(&c,0,0);s_motion.drawWheel(c,ms);ui::onboarding_approved::wheelText(c,ms);
  }else ui::onboarding_approved::navigation(c,18,ms,0);
 }
 else if(s_flow.page==18||(s_flow.page>=24&&s_flow.page<=26)){
  const unsigned offset=s_text.scroll==999?s_text.follow(dialogueCopy(),ms<450?0:(ms-450)/56,textRows()):s_text.scroll;
  ui::onboarding_approved::navigation(c,s_flow.page,ms,offset);
 }
 else if(s_flow.page==14){
  c.fillSprite(kUiSuccessBg);c.drawCircle(140,90,23,kUiFg);
  c.drawLine(128,90,137,99,kUiFg);c.drawLine(137,99,153,81,kUiFg);
  ui::design::center(c,"Connected",126);onboarding_wifi::arrows(c,false,true);
 }else{
  Flow a=s_flow,b=s_flow;a.input(Input::A);b.input(Input::B);
  const bool back=s_flow.page<3?s_flow.page!=0:a.page!=s_flow.page,next=b.page!=s_flow.page;
  const unsigned offset=s_text.scroll==999?s_text.follow(dialogueCopy(),ms<450?0:(ms-450)/56,textRows()):s_text.scroll;
  const int letters=std::min<unsigned>(s_wrapped.size(),ms<450?0:(ms-450)/56);
  const int key=int(offset)*2+int(ms%4200>4010);
  const bool animated=s_flow.page>=3&&s_flow.page<=6;
  if(!animated&&!s_dialogueCache.created()){s_dialogueCache.setColorDepth(16);s_dialogueCache.createSprite(280,240);}
  if(!animated&&s_dialogueCache.created()){
   // Scrollbar thumb extends outside the text clear rectangle. Rebuild its
   // background on scroll changes so the old thumb cannot remain behind.
   if(s_cachePage!=int(s_flow.page)||s_cacheScroll!=key){
    ui::onboarding_approved::background(s_dialogueCache,s_flow.page,ms,88,120,back,next);
    s_cacheLetters=-1;
   }
   if(s_cachePage!=int(s_flow.page)||s_cacheLetters!=letters||s_cacheScroll!=key){
    s_dialogueCache.fillRect(34,132,212,80,kUiBg);
    ui::onboarding_approved::draftText(s_dialogueCache,s_wrapped,letters,offset,120,true);
    s_cachePage=s_flow.page;s_cacheLetters=letters;s_cacheScroll=key;
   }
   s_dialogueCache.pushToSprite(&c,0,0);
  }else ui::onboarding_approved::proposal(c,s_flow.page,ms,offset,88,120,ui::onboarding_approved::copy(s_flow.page),back,next);
 }
 s_drawUs=micros()-started;hal::display::markAllDirty();
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
bool navigationPracticed(){return navigationLessonActive()&&s_flow.practiced;}
void noteNavigation(){if(navigationLessonActive())s_flow.practiced=true;}
bool returnFromNavigation(){
 if(!navigationLessonActive())return false;
 if(!s_flow.practiced)return true;
 auto& nav=NavHandler::instance();
 if(nav.top()==&apps::menu::app)return false; // Menu owns its own category history.
 if(nav.depth()>1&&nav.top()!=&app)nav.pop();
 return true;
}
static void drawNavigationHint(TFT_eSprite& c){
 if(!navigationLessonActive())return;
 const int style=s_flow.practiced?1:0,top=style?164:194,h=240-top;
 if(!s_navHint.created()||s_hintStyle!=style){
  s_navHint.deleteSprite();s_navHint.setColorDepth(16);s_navHint.createSprite(280,h);
  if(!s_navHint.created())return;
  struct Offset{TFT_eSprite& c;int top;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y-top,w,h,ink);}} offset{s_navHint,top};
  s_navHint.fillSprite(kUiBg);ui::onboarding_approved::menuInstruction(offset,s_flow.practiced);s_hintStyle=style;
 }
 if(hal::display::isOptimizedRender()){hal::display::beginDirectDraw();s_navHint.pushSprite(0,top);hal::display::endDirectDraw();}
 else{s_navHint.pushToSprite(&c,0,top);hal::display::markDirty(top,h);}
}

void replay(){
 NavHandler::instance().setWorkflowPolicy({});
 s_talkPending=s_agentStarted=false;
 auto& nav=NavHandler::instance();nav.popToRoot();load();s_flow.reset();s_active=true;s_bootChecked=true;
 services::notifyUserActivity();nav.push(&app);
}
void beginWifiSetup(){
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
void printStatus(){const auto audio=sfx::typewriterStats();Serial.printf("[tutorial] page=%u elapsed=%u ready=%u active=%u practiced=%u completed=%u reveal=%u scroll=%u agent=%u recording=%u thinking=%u reply=%u error=%u drawUs=%lu fps=%.1f saved=%08lx speechOpen=%u speechBlips=%u audioBlips=%lu audioAvgGap=%lu audioMaxGap=%lu motion=%u\n",s_flow.page,elapsed(),readyAt(),isActive(),s_flow.practiced,s_completed,s_text.offset,s_text.scroll,s_talkPending,apps::agent::isRecording(),apps::agent::isSending(),apps::agent::isOnReplyScreen(),apps::agent::isShowingError(),(unsigned long)s_drawUs,hal::display::currentFps(),(unsigned long)services::onboarding_progress::savedRecordForDevelopment(),s_speechOpen,s_speechBlips,(unsigned long)audio.played,(unsigned long)(audio.played>1?audio.totalGapMs/(audio.played-1):0),(unsigned long)audio.maxGapMs,s_motionReady);}
bool debugSeek(unsigned page,unsigned ms){
 if(!isActive()||page>34||(page>=12&&page<=15)||page==19||page==20||page==23||page==27||page==30||page==31)return false;
 stopSpeech();s_speechLetters=s_speechBlips=0;releaseDialogue();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_flow.page=page==26?25:page;refreshCopy();s_scanAfter=readyAt()+kScanReadingPauseMs;s_text.scroll=0;s_frozen=true;s_frozenMs=ms;prepareMotion();s_dirty=true;return true;
}
void debugPlay(){s_started=millis()-elapsed();s_frozen=false;s_dirty=true;}
#endif
}} // namespace apps::tutorial
