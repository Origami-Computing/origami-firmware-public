#include "apps/tutorial_app.h"
#include "apps/agent_app.h"
#include "apps/onboarding_flow.h"
#include "services/onboarding_progress.h"
#include "apps/menu_app.h"
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
#include <Arduino.h>

namespace apps { namespace tutorial {
namespace {
using onboarding::Input;
using onboarding::Flow;
Flow s_flow;
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
int s_cachePage=-1,s_cacheLetters=-1,s_cacheScroll=-1;
void releaseDialogue(){s_dialogueCache.deleteSprite();s_cachePage=s_cacheLetters=-1;}
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
const char* dialogueCopy(){return s_flow.page<=6?ui::onboarding_intro::dialogue(s_flow.page):s_flow.page>=24&&s_flow.page<=26?onboarding_navigation::talkCopy(s_flow.page):onboarding_wifi::copy(s_flow.page);}
unsigned textRows(){return s_flow.page==25?2:s_flow.page==9?6:4;}
unsigned readyAt(){if(s_flow.page==14)return 0;if(s_flow.page>=24&&s_flow.page<=26)return onboarding_navigation::talkReady(s_flow.page);return s_flow.page<=6?ui::onboarding_intro::readyAt(s_flow.page):onboarding_wifi::ready(s_flow.page);}
void serviceSpeech(unsigned ms){
 const char* copy=dialogueCopy();
 const unsigned starts=s_flow.page==0?ui::onboarding_intro::kTextAt:450;
 const unsigned count=ms<starts?0:std::min(unsigned(strlen(copy)),(ms-starts)/ui::onboarding_intro::kCharacterMs);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 if(s_frozen){stopSpeech();s_speechLetters=count;return;}
#endif
 if(s_text.offset||!copy[0]){stopSpeech();s_speechLetters=count;return;}
 if(count>s_speechLetters){
  // Match agent replies: play the latest revealed character, never a backlog
  // of blips after a slow frame. Scrolling already-visible text stays silent.
  const char ch=copy[count-1];
  if(ch>' '){
   sfx::typewriterServiceChar(ch,count-1);
   s_speechOpen=sfx::typewriterIsOpen();
   if(s_speechOpen)++s_speechBlips;
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
 stopSpeech();s_speechLetters=s_speechBlips=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;
#endif
 releaseDialogue();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_scanAfter=readyAt()+kScanReadingPauseMs;
 s_started=millis();s_lastDraw=0;s_dirty=true;
}
void changed(){
 stopSpeech();s_speechLetters=s_speechBlips=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;
#endif
 releaseDialogue();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_scanAfter=readyAt()+kScanReadingPauseMs;s_started=millis();s_lastDraw=0;s_dirty=true;save();}
void prepareWorkflow(){
 if(!s_active){s_active=true;load();s_flow.reset();}
 else if(s_flow.page==12)s_flow.wifiReturned(apps::wifi_setup::takeTutorialResult());
 else if(s_flow.page==19)s_flow.menuReturned();
 save();
}
void exitApp(){stopSpeech();s_active=false;releaseDialogue();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 s_frozen=false;
#endif
}
void startScanner(){
 s_flow.page=12;changed();apps::wifi_setup::beginTutorial();
}
void tick(uint32_t now){
 if(s_flow.wifiOnly && s_flow.page==Flow::kFinished){NavHandler::instance().pop();return;}
 if(now-s_lastDraw>=33)s_dirty=true;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 if(s_frozen)return;
#endif
 if(s_flow.page==9&&elapsed()+s_text.offset>=s_scanAfter)startScanner();
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
 const unsigned age=elapsed(),starts=s_flow.page==0?ui::onboarding_intro::kTextAt:450;
 if(input==Input::B&&s_flow.page!=18&&s_text.reveal(age,starts,readyAt())){stopSpeech();s_dirty=true;return;}
 if(input==Input::Rotate&&s_flow.page>=7&&s_flow.page!=18){
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
 if(s_flow.page<=6)ui::onboarding_intro::render(c,ms,s_flow.page);
 else if(s_flow.page==18||(s_flow.page>=24&&s_flow.page<=26))drawHome(c,ms);
 else if(s_flow.page==14){
  c.fillSprite(kUiSuccessBg);c.drawCircle(140,90,23,kUiFg);c.drawCircle(140,90,22,kUiFg);
  for(int i=-1;i<=1;++i){c.drawLine(128,90+i,137,99+i,kUiFg);c.drawLine(137,99+i,153,81+i,kUiFg);}
  ui::pixel_text::centered(c,"Connected",126,2);onboarding_wifi::arrows(c,false,true);
 }
 else {
  // Text is static between character reveals. Cache that layer in PSRAM so
  // the subtle hand/prompt animation never rerasterizes five dense text lines.
  const unsigned ready=onboarding_wifi::ready(s_flow.page);
  const unsigned textMs=std::min(ms,ready-1);
  const int scrollKey=s_flow.page==8?websiteOffset():int(s_text.scroll);
  const int letters=textMs<450?0:(textMs-450)/ui::onboarding_intro::kCharacterMs;
  if(!s_dialogueCache.created()){s_dialogueCache.setColorDepth(16);s_dialogueCache.createSprite(280,240);}
  if(s_dialogueCache.created()){
    if(s_cachePage!=int(s_flow.page)||s_cacheLetters!=letters||s_cacheScroll!=scrollKey){
      onboarding_wifi::dialogue(s_dialogueCache,s_flow.page,textMs,s_text.scroll,true,scrollKey);s_cachePage=s_flow.page;s_cacheLetters=letters;s_cacheScroll=scrollKey;
    }
    s_dialogueCache.pushToSprite(&c,0,0);
    // Keep the animated footer independent of the static text cache.
    if(s_flow.page==8)onboarding_wifi::websiteArrow(c,ms,scrollKey);
    if(s_flow.page!=8&&s_flow.page!=9){
    c.fillRect(0,188,280,52,kUiBg);
    if(ms<ready){if(ms>=450)onboarding_wifi::footer(c,ms-450,"Press B to skip");}
    else if(s_flow.page==28)onboarding_wifi::footer(c,ms-ready,"A: No / B: Yes");
    }
  }else onboarding_wifi::dialogue(c,s_flow.page,ms,s_text.scroll,true,scrollKey);
  if(s_flow.page>=17){clock(c);if(s_flow.page==21||s_flow.page==22)c.fillRect(20,10,36,25,kUiBg);}
 }
 serviceSpeech(ms);
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
void noteNavigation(){if(navigationLessonActive())s_flow.practiced=true;}
static void drawNavigationHint(TFT_eSprite& c){
 if(!navigationLessonActive())return;
 // Cache the antialiased shoulder icons once, without spending animation time
 // redrawing their quadratic coverage over every frame of a real app.
 static TFT_eSprite hint(&hal::display::directTft());static int style=-1;
 const int nextStyle=s_flow.practiced?1:0;
 if(!hint.created()){hint.setColorDepth(16);hint.createSprite(280,72);}
 if(!hint.created())return;
 if(style!=nextStyle){
  struct Offset {TFT_eSprite& c;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y-168,w,h,ink);}} offset{hint};
  hint.fillSprite(kUiBg);onboarding_navigation::instruction(offset,s_flow.practiced);style=nextStyle;
 }
 if(hal::display::isOptimizedRender()){
  hal::display::beginDirectDraw();hint.pushSprite(0,168);hal::display::endDirectDraw();
 }else {hint.pushToSprite(&c,0,168);hal::display::markDirty(168,72);}
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
void printStatus(){Serial.printf("[tutorial] page=%u elapsed=%u ready=%u active=%u practiced=%u completed=%u reveal=%u scroll=%u agent=%u recording=%u thinking=%u reply=%u error=%u drawUs=%lu fps=%.1f saved=%08lx speechOpen=%u speechBlips=%u\n",s_flow.page,elapsed(),readyAt(),isActive(),s_flow.practiced,s_completed,s_text.offset,s_text.scroll,s_talkPending,apps::agent::isRecording(),apps::agent::isSending(),apps::agent::isOnReplyScreen(),apps::agent::isShowingError(),(unsigned long)s_drawUs,hal::display::currentFps(),(unsigned long)services::onboarding_progress::savedRecordForDevelopment(),s_speechOpen,s_speechBlips);}
bool debugSeek(unsigned page,unsigned ms){
 if(!isActive()||page>28||(page>=12&&page<=15)||page==19||page==20||page==23||page==27)return false;
 stopSpeech();s_speechLetters=s_speechBlips=0;releaseDialogue();s_text.reset();s_scrollFrom=s_scrollTarget=0;s_scrollAt=millis();s_flow.page=page==26?25:page;s_scanAfter=readyAt()+kScanReadingPauseMs;s_frozen=true;s_frozenMs=ms;s_dirty=true;return true;
}
void debugPlay(){s_started=millis()-elapsed();s_frozen=false;s_dirty=true;}
#endif
}} // namespace apps::tutorial
