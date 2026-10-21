/** Chrome-style runner. Artwork and mechanics share the landscape viewport. */
#ifdef MENU_APP
#include "apps/trex_app.h"
#include "assets/trex_chromium.h"
#include "games/trex_runner.h"
#include "hal/hal_display.h"
#include "scene/screen.h"
#include "ui/scene.h"
#include "ui/screen_chrome.h"
#include <Arduino.h>
#include <esp_random.h>
#include <cstdio>
#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include "core/watchdog.h"
#endif

namespace apps { namespace trex {
using games::trex::Kind;
static games::trex::Runner s;
static uint32_t lastMs=0,drawUs=0;
static bool active=false;
constexpr uint16_t kBg=0xffff,kInk=0x528a;
static void sprite(TFT_eSprite&c,const trex_art::Sprite&a,int x,int y){
 for(unsigned i=0;i<a.count;++i){const auto&r=a.runs[i];const int yy=y+r.y;
   if(yy<38||yy>=c.height())continue;
   const int left=std::max(0,x+int(r.x)),right=std::min(int(c.width()),x+int(r.x+r.w));
   if(right>left)c.drawFastHLine(left,yy,right-left,r.color);
 }
}
static void text(TFT_eSprite&c,const char*t,int y,int font=2){c.setTextColor(kInk,kBg);c.drawCentreString(t,c.width()/2,y,font);}
static void score(TFT_eSprite&c){
 char value[32];snprintf(value,sizeof(value),"HI %05lu %05lu",(unsigned long)(s.best%100000),(unsigned long)(s.score%100000));
 int x=c.width()-int(strlen(value))*11-8;
 for(const char*p=value;*p;++p,x+=11){int i=*p>='0'&&*p<='9'?*p-'0':*p=='H'?10:*p=='I'?11:-1;if(i>=0)sprite(c,*trex_art::chars[i],x,40);}
}
void enter(){active=true;s.width=hal::display::canvas().width();s.height=hal::display::canvas().height();s.rng=esp_random();s.reset();lastMs=millis();Serial.println("[trex] enter");}
void exitApp(){active=false;Serial.println("[trex] exit");}
void tick(){const uint32_t now=millis();s.advance(now-lastMs);lastMs=now;}
void onDraw(TFT_eSprite&c){
 const uint32_t start=micros();setSceneChromeBackground(kBg);hal::display::clearContentArea(kBg);
 const int g=int(s.ground()),scroll=int(s.distance)%600;
 sprite(c,trex_art::cloud,115-int(s.distance*.10f)%350,83);sprite(c,trex_art::cloud,236-int(s.distance*.07f)%370,105);
 sprite(c,trex_art::horizon,-scroll,g-3);sprite(c,trex_art::horizon,600-scroll,g-3);
 for(const auto&o:s.obstacles){if(!o.active)continue;const int x=int(o.x);
   switch(o.kind){
    case Kind::Small:sprite(c,trex_art::small,x,g-35);break;
    case Kind::Tall:sprite(c,trex_art::tall,x,g-50);break;
    case Kind::Pair:sprite(c,trex_art::small,x,g-35);sprite(c,trex_art::small,x+17,g-35);break;
    case Kind::Bird:sprite(c,(s.simMs/167)%2?trex_art::bird1:trex_art::bird0,x,g-65);break;
   }
 }
 const trex_art::Sprite*d=&trex_art::dino0;
 if(s.dead)d=&trex_art::dino3;
 else if(s.duck&&s.grounded)d=(s.simMs/125)%2?&trex_art::duck1:&trex_art::duck0;
 else if(s.grounded&&!s.ready)d=(s.simMs/83)%2?&trex_art::dino2:&trex_art::dino1;
 sprite(c,*d,int(games::trex::kPlayerX),g-47+int(s.y));score(c);
 if(s.ready){text(c,"T - R E X",90);text(c,"press B to start",119);text(c,"B jump  A duck  C pause",220,1);}
 else if(s.paused){c.fillRect(45,89,c.width()-90,55,kBg);text(c,"P A U S E D",95);text(c,"press dial to resume",122,1);}
 else if(s.dead){text(c,"G A M E  O V E R",94);text(c,"press dial to play again",124,1);}
 hal::display::markAllDirty();drawUs=micros()-start;
}
void onInput(hal::input::InputEvent ev){
 using E=hal::input::EventType;
 if(ev.type==E::ButtonC_Tap){s.dial();lastMs=millis();return;}
 if(ev.type==E::ButtonB_Release){s.releaseJump();return;}
 if(ev.type==E::ButtonA_Release){s.releaseDuck();return;}
 if(s.paused)return;
 if(ev.type==E::ButtonB_Press){s.pressJump();lastMs=millis();}
 if(ev.type==E::ButtonA_Press){if(s.dead)s.reset(false);else s.pressDuck();}
 // Tap follows Release in the HAL. It must never initiate a second jump.
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
// The expanded synchronous diagnostic exceeds one UI watchdog interval.
// A completed bounded check is progress; a stuck check still times out.
static void testProgress(){core::watchdog::feed();delay(1);}
void printStatus(Print&out){
 char b[360];int count=0;float nearest=999;for(const auto&o:s.obstacles)if(o.active){++count;nearest=std::min(nearest,o.x);}
 snprintf(b,sizeof(b),"[trex] status active=%d mode=%s width=%d height=%d y=%.2f vy=%.2f speed=%.2f distance=%.2f score=%lu sim_ms=%lu jumps=%lu landings=%lu grounded=%d duck=%d obstacles=%d nearest=%.2f spawn=%.2f draw_us=%lu\n",active,s.ready?"ready":s.dead?"game-over":s.paused?"paused":"running",s.width,s.height,s.y,s.vy,s.speed,s.distance,(unsigned long)s.score,(unsigned long)s.simMs,(unsigned long)s.jumps,(unsigned long)s.landings,s.grounded,s.duck,count,nearest,s.nextSpawn,(unsigned long)drawUs);out.print(b);
}
void testPhysics(Print&out){
 using games::trex::Runner;using games::trex::Obstacle;
 unsigned checks=0,failures=0;
 auto check=[&](bool ok,const char*name){++checks;if(!ok)++failures;out.print("[trex-test] ");out.print(ok?"PASS ":"FAIL ");out.println(name);testProgress();};
 auto empty=[](){Runner r;r.reset(false);r.nextSpawn=1e9;return r;};
 auto atSpeed=[&](float speed){Runner r=empty();
   const float delta=std::min(speed-games::trex::kBaseSpeed,169.8f);
   r.simMs=uint32_t(90000.f*delta/(170.f-delta));r.speed=r.targetSpeed();return r;};
 Runner held=empty();held.pressJump();float apex=0;while(!held.grounded&&held.simMs<2000){held.advance(8);apex=std::max(apex,-held.y);}
 check(held.simMs>=568&&held.simMs<=584&&apex>61&&apex<63,"held jump 0.57s / 62px");
 Runner shortHop=empty();shortHop.pressJump();shortHop.releaseJump();float shortApex=0;while(!shortHop.grounded&&shortHop.simMs<2000){shortHop.advance(8);shortApex=std::max(shortApex,-shortHop.y);}
 check(shortHop.simMs<held.simMs&&shortApex>36&&shortApex<51,"release shortens after safe minimum rise");
 held.releaseJump();held.advance(32);check(held.grounded&&held.jumps==1,"release after landing never jumps");
 Runner paused=empty();paused.pressJump();paused.advance(80);paused.dial();const float py=paused.y,pd=paused.distance;const auto pt=paused.simMs;
 paused.advance(5000);check(paused.y==py&&paused.distance==pd&&paused.simMs==pt,"pause freezes simulation and spawn distance");paused.dial();paused.advance(8);check(paused.simMs==pt+8,"resume advances one step without catch-up");
 Runner a=empty(),b=empty();a.pressJump();b.pressJump();for(int i=0;i<100;++i)a.advance(8);for(int i=0;i<8;++i)b.advance(100);
 check(a.y==b.y&&a.distance==b.distance&&a.simMs==b.simMs,"10 FPS and 125 FPS identical simulation");
 Runner spawn;spawn.reset(false);spawn.nextSpawn=0;spawn.advance(8);check(spawn.obstacles[0].active&&spawn.obstacles[0].x==292,"spawns beyond actual 280px screen");
 Runner bird=empty();Obstacle o{true,24,Kind::Bird};check(bird.collides(o),"bird hits standing dinosaur");bird.pressDuck();check(!bird.collides(o),"duck clears bird collision silhouette");
 Runner drop=empty();drop.pressJump();drop.advance(80);drop.pressDuck();check(drop.vy>=360,"A starts immediate fast fall");
 for(float speed:{150.f,190.f,235.f,280.f,320.f})for(Kind kind:{Kind::Small,Kind::Tall,Kind::Pair}){
   unsigned wins=0;
   for(int lead=1;lead<=180;++lead){if(lead%15==0)delay(0);Runner r=atSpeed(speed);r.obstacles[0]={true,24.f+lead,kind};r.pressJump();
     for(int t=0;t<1200&&!r.dead;++t){r.advance(8);if(r.obstacles[0].x+games::trex::width(kind)<24)break;}
     if(!r.dead)++wins;
   }
   char label[100];snprintf(label,sizeof(label),"jump window speed=%.0f kind=%u winning_leads=%u",speed,unsigned(kind),wins);check(wins>=unsigned(speed*.08f),label);
 }
 for(float speed:{150.f,190.f,235.f,280.f,320.f}){
   Runner r=atSpeed(speed);const auto start=r.simMs;
   r.obstacles[0]={true,180,Kind::Small};
   r.obstacles[1]={true,180+r.spacing(Kind::Small),Kind::Tall};
   r.obstacles[2]={true,r.obstacles[1].x+r.spacing(Kind::Tall),Kind::Pair};
   r.obstacles[3]={true,r.obstacles[2].x+r.spacing(Kind::Pair),Kind::Bird};
   unsigned recoveredAt=0,previousLandings=0;
   while(r.simMs-start<12000&&!r.dead){
     int nearest=-1;for(int i=0;i<4;++i)if(r.obstacles[i].active&&r.obstacles[i].x+games::trex::width(r.obstacles[i].kind)>24&&(nearest<0||r.obstacles[i].x<r.obstacles[nearest].x))nearest=i;
     if(nearest<0)break;
     if(r.landings!=previousLandings){previousLandings=r.landings;recoveredAt=r.simMs+160;}
     if(r.grounded&&r.simMs>=recoveredAt){
       if(r.obstacles[nearest].kind==Kind::Bird)r.pressDuck();
       else {r.releaseDuck();Runner trial=r;trial.pressJump();while(!trial.grounded&&!trial.dead)trial.advance(8);
         if(!trial.dead&&trial.obstacles[nearest].x+games::trex::width(trial.obstacles[nearest].kind)<24)r.pressJump();}
     }
     r.advance(8);if(r.simMs%256==0)delay(0);
   }
   char label[100];snprintf(label,sizeof(label),"mixed sequence speed=%.0f with 160ms landing recovery",speed);check(!r.dead&&r.obstacles[3].x+46<24,label);
 }
 for(float speed:{150.f,190.f,235.f,280.f,320.f}){
   unsigned wins=0;
   for(int lead=1;lead<150;++lead){Runner r=atSpeed(speed);const auto start=r.simMs;r.obstacles[0]={true,24.f+lead,Kind::Small};r.pressJump();r.releaseJump();
     while(r.simMs-start<2000&&!r.dead&&r.obstacles[0].x+17>=24)r.advance(8);
     if(!r.dead)++wins;
   }
   char label[100];snprintf(label,sizeof(label),"short tap clears small cactus speed=%.0f winning_leads=%u",speed,wins);check(wins>=unsigned(speed*.08f),label);
 }
 // Exercise every ordered transition, including bird -> cactus, with real
 // collision geometry and a deliberate human-sized recovery after landing.
 auto playStep=[](Runner&r,uint32_t&recover,unsigned&landings){
   int nearest=-1;for(int i=0;i<5;++i)if(r.obstacles[i].active&&r.obstacles[i].x+games::trex::width(r.obstacles[i].kind)>24&&
      (nearest<0||r.obstacles[i].x<r.obstacles[nearest].x))nearest=i;
   if(r.landings!=landings){landings=r.landings;recover=r.simMs+160;}
   if(nearest>=0&&r.grounded&&r.simMs>=recover){
     if(r.obstacles[nearest].kind==Kind::Bird)r.pressDuck();
     else {r.releaseDuck();Runner trial=r;trial.nextSpawn=1e9;trial.pressJump();
       while(!trial.grounded&&!trial.dead)trial.advance(8);
       if(!trial.dead&&trial.obstacles[nearest].x+games::trex::width(trial.obstacles[nearest].kind)<24)r.pressJump();}
   }
   r.advance(8);
 };
 for(float speed:{150.f,190.f,235.f,280.f,320.f})for(unsigned first=0;first<4;++first)for(unsigned second=0;second<4;++second){
   Runner r=atSpeed(speed);const auto start=r.simMs;uint32_t recover=0;unsigned landings=0;
   r.obstacles[0]={true,200,Kind(first)};r.obstacles[1]={true,200+r.spacing(Kind(first)),Kind(second)};
   while(!r.dead&&r.simMs-start<8000&&r.obstacles[1].x+games::trex::width(Kind(second))>=24)playStep(r,recover,landings);
   char label[100];snprintf(label,sizeof(label),"transition %.0f %u->%u with 160ms recovery",speed,first,second);
   check(!r.dead&&r.obstacles[1].x+games::trex::width(Kind(second))<24,label);delay(0);
 }
 Runner curve=empty();float previous=0;
 for(uint32_t age:{0u,60000u,120000u,300000u,600000u,1800000u}){
   curve.simMs=age;curve.advance(8);check(curve.speed>previous&&curve.speed<games::trex::kMaxSpeed,"speed keeps increasing through 30 minutes");previous=curve.speed;
 }
 curve.best=123;curve.reset();check(curve.speed==150&&curve.simMs==0&&curve.repeats==0&&curve.best==123,"restart resets difficulty and keeps best");
 for(uint32_t age:{0u,12000u,25000u,45000u,600000u}){
   Runner r=empty();r.simMs=age;unsigned seen=0;bool valid=true;
   for(int i=0;i<1000;++i){Kind k=r.chooseKind();seen|=1u<<unsigned(k);
     valid&=(age>=12000||k==Kind::Small)&&(age>=25000||k!=Kind::Pair)&&(age>=45000||k!=Kind::Bird)&&(age<45000||r.repeats<=2);}
   const unsigned expected=age<12000?1:age<25000?3:age<45000?7:15;
   check(valid&&seen==expected,"gradual obstacle unlocks and no long late-game repeats");
 }
 for(uint32_t age:{0u,300000u,1800000u})for(uint32_t seed:{1u,42u}){
   Runner r;r.reset(false);r.simMs=age;r.speed=r.targetSpeed();r.rng=seed;
   uint32_t recover=0;unsigned landings=0;
   while(!r.dead&&r.simMs-age<180000){playStep(r,recover,landings);if(r.simMs%256==0)delay(0);}
   char label[100];snprintf(label,sizeof(label),"generated 3-minute run age=%lu seed=%lu",(unsigned long)age,(unsigned long)seed);
   check(!r.dead&&r.landings>20,label);
 }
 const Runner saved=s;const auto savedMs=lastMs;s=empty();s.pressJump();s.advance(200);s.advance(200);s.advance(200);
 hal::input::InputEvent event{};event.type=hal::input::EventType::ButtonB_Tap;onInput(event);check(s.jumps==1&&s.grounded,"HAL Tap ignored by actual app dispatcher");s=saved;lastMs=savedMs;
 char result[120];snprintf(result,sizeof(result),"[trex] tests checks=%u failures=%u held_ms=%lu short_ms=%lu\n",checks,failures,(unsigned long)held.simMs-32,(unsigned long)shortHop.simMs);out.print(result);
}
#endif
static void screenTick(uint32_t){tick();}
static void buildScene(ui::scene::Scene&scene,int16_t w,int16_t h){scene.clear();scene.addRect("trex.bg",{0,38,w,int16_t(h-38)},kBg,0,0);scene.addLabel("trex.title",{0,90,w,24},"T-Rex Game",kInk,kBg,2,ui::scene::TextAlign::TopLeft,1);}
static const ::scene::Screen screen={"T-Rex Game",nullptr,nullptr,screenTick,onInput,nullptr,onDraw,"game",buildScene};
static const ::scene::Screen*const screens[]={&screen};
App app=SCENE_APP_CATALOG_LIFECYCLE_HOOKS("T-Rex Game","trex",&screen,enter,enter,exitApp,"src/apps/trex_app.cpp",screens,nullptr,nullptr);
} }
#endif
