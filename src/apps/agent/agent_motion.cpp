#include "apps/agent/agent_motion.h"
#include "services/agent_voice.h"
#include "apps/agent/agent_render.h"
#include "apps/agent/agent_emotion.h"
#include "apps/agent_app.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include "ui/agent_transition_motion.h"
#include "ui/eye_rig.h"
#include "ui/home_eyes.h"
#include "ui/kami_motion.h"
#include "ui/kami_dialogue.h"
#include "ui/listening_style.h"
#include "ui/music_kami_view.h"
#include "ui/pixel_dialogue_frame.h"
#include "config/ui_layout.h"
#include <esp_heap_caps.h>

namespace apps { namespace agent { namespace motion {
namespace {
enum class Kind { None, MenuHome, ReplyHome, Listening, Thinking, Reply, CenteredHome };
Kind kind=Kind::None;
uint32_t started=0, lastFrame=0, frames=0, maxUs=0;
uint16_t* previous=nullptr;
bool homePrepared=false,contentTrimmed=false,homeFromCentered=false;
bool listeningFromReply=false,thinkingSettled=false;
float listeningX=0,listeningY=0;
int motionTop=40,motionBottom=214,dirtyTop=40,dirtyBottom=214;
Stage homeFrom=Stage::Eyes;
float releaseAvatar=1, releaseBody=1, releaseListening=1, releaseGlow=1;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
int heldAge=-1;
#endif
unsigned ageAt(uint32_t now){
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    if(heldAge>=0)return unsigned(heldAge);
#endif
    // A screen can be entered after this UI loop sampled `now`. Do not let
    // that older tick underflow to ~49 days and complete the animation early.
    const int32_t elapsed=int32_t(now-started);
    return elapsed>0?unsigned(elapsed):0u;
}
ui::eye_rig::Pose pose=ui::eye_rig::kPresets[0].pose;
uint16_t swap(uint16_t x){return (x>>8)|(x<<8);}
int mix(int a,int b,float f){return lroundf(a+(b-a)*f);}
uint16_t fade(uint16_t c,float f){
    const int a=lroundf(f*255);
    return (((((c>>11)&31)*a+31*(255-a)+127)/255)<<11)|
           (((((c>>5)&63)*a+63*(255-a)+127)/255)<<5)|
           (((c&31)*a+31*(255-a)+127)/255);
}
struct Ink {
    float alpha;
    void fillRect(int x,int y,int w,int h,uint16_t c){hal::display::tft().fillRect(x,y,w,h,fade(c,alpha));}
};
void begin(Kind next){
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    heldAge=-1;
#endif
    kind=next;started=millis();lastFrame=0;frames=0;maxUs=0;contentTrimmed=false;motionTop=dirtyTop=40;motionBottom=dirtyBottom=214;
    Serial.printf("[agent-motion] begin=%d\n",int(kind));
}
void snapshot(){
    free(previous);
    previous=static_cast<uint16_t*>(heap_caps_malloc(280*240*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(previous)memcpy(previous,hal::display::canvas().getPointer(),280*240*2);
}
void restore(float opacity,int firstRow,int lastRow=240,int firstCol=0,int lastCol=280){
    if(!previous || opacity<=0)return;
    auto* out=static_cast<uint16_t*>(hal::display::canvas().getPointer());
    for(int y=firstRow;y<lastRow;++y)for(int x=firstCol;x<lastCol;++x){
        int i=y*280+x;
        // The destination was cleared to white already. Most snapshot pixels
        // are white; don't blend and rewrite the entire screen for a few glyphs.
        const uint16_t pixel=previous[i];
        if(pixel!=0xffff)out[i]=opacity>=.999f?pixel:swap(fade(swap(pixel),opacity));
    }
}
void actor(const ui::agent_motion::Frame& f,bool capturing=false){
    Ink body{f.body};
    if(f.body>0)music_kami::mask(body,assets::kami_dialogue::kBody,26,40,24,26,0xF945,0xffff);
    float a=f.avatar;
    if(a>=.999f && f.eyes>=.999f && f.body>=.999f){
        ui::kami_dialogue::drawAvatarPose(hal::display::tft(),26,40,true,pose);
        return;
    }
    // A shared neutral silhouette during travel avoids rerasterizing large
    // geometry on every frame. The settled mini avatar owns reply expressions.
    if(f.eyes>=.999f && (f.body<=0 || a<=.9f) &&
       ui::home_eyes::drawTravelEyes(hal::display::tft(),a,f.offsetX,f.offsetY))return;
    const int h=mix(170,10,a);
    // Band live capture to drain its 32 ms DMA ring. Before capture starts,
    // rasterize once: repeated geometry setup delays the first acknowledgment.
    const int band=capturing?8:h;
    for(int row=0;row<h;row+=band){
        ui::eye_rig::draw(hal::display::tft(),mix(14,30,a),mix(42,47,a),mix(252,16,a),h,
            pose,1,a>.9f?fade(0xF945,f.body):0xffff,88.f+(16.f/2.2f-88.f)*a,f.offsetX,f.offsetY,false,uint8_t(f.eyes*255),row,std::min(row+band,h),.9f+.1f*a);
        serviceMicCaptureEarly();
    }
}
void listening(float alpha,uint32_t now){
    if(alpha<=0)return;
    Ink ink{alpha};music_view::text(ink,"Listening",68,204,2,0x18c3);
    for(int i=0;i<3;++i){
        int lift=lroundf(4*fmaxf(0.f,sinf(now*6.2831853f/1200-i*.8f)));
        music_view::text(ink,".",176+i*12,204-lift,2,0x18c3);
    }
}
void finishFrame(uint32_t us,bool full=true){
    if(full){hal::display::markAllDirty();NavHandler::instance().invalidateChrome();}
    else hal::display::markDirty(dirtyTop,dirtyBottom-dirtyTop);
    ++frames;maxUs=std::max(maxUs,uint32_t(micros()-us));
}
}
void status(){Serial.printf("[agent-motion] kind=%d age=%lu frames=%lu max_us=%lu\n",int(kind),
    (unsigned long)(millis()-started),(unsigned long)frames,(unsigned long)maxUs);}
bool ownsStatusBackground(){return (kind==Kind::Listening || kind==Kind::Thinking) && ui::listening_style::ready();}
void cancel(){free(previous);previous=nullptr;kind=Kind::None;homePrepared=false;ui::listening_style::end();hal::display::setFlushService(nullptr);}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool seek(int ms){
    if(ms < -1 || ms > 10000 || kind==Kind::None)return false;
    if(ms==-1 && heldAge>=0)started=millis()-heldAge;
    heldAge=ms;lastFrame=0;return true;
}
#endif
void beforeStage(Stage from,Stage to){
    if(to==Stage::Recording)listeningFromReply=from==Stage::Reply;
    if(to==Stage::Eyes){
        homeFromCentered=from==Stage::Recording || from==Stage::Thinking || kind==Kind::Listening || kind==Kind::Thinking;
        homeFrom=from;homePrepared=true;snapshot();
        pose=from==Stage::Reply?emotion::snapshot(0).pose:ui::eye_rig::kPresets[0].pose;
    } else if(to==Stage::Thinking && from==Stage::Recording){
        auto f=ui::agent_motion::homeListen(ageAt(millis()));
        const unsigned age=ageAt(millis());
        releaseAvatar=listeningFromReply?1-ui::agent_motion::ramp(age,0,420):0;
        releaseBody=listeningFromReply?1-ui::agent_motion::ramp(age,0,160):0;
        releaseListening=f.listening;releaseGlow=f.glow;
    } else if(to!=Stage::Thinking){cancel();}
}
void prepareHome(){
    if(!homePrepared){homeFromCentered=false;homeFrom=Stage::Eyes;snapshot();pose=ui::eye_rig::kPresets[0].pose;}
}
void startHome(){homePrepared=false;begin(homeFromCentered?Kind::CenteredHome:homeFrom==Stage::Eyes?Kind::MenuHome:Kind::ReplyHome);}
void startListening(uint32_t now,float x,float y){
    cancel();pose=ui::eye_rig::kPresets[0].pose;begin(Kind::Listening);started=now;
    listeningX=x;listeningY=y;
    if(listeningFromReply)snapshot();
    ui::kami_motion::reset();ui::home_eyes::setHomeMotion(0,false);
    ui::home_eyes::setShellBlinkFrozen(false);ui::home_eyes::setEmotion("neutral",true);
    ui::home_eyes::resetShellBlink();
    hal::display::setFlushService(serviceMicCaptureEarly);
    // Cache-backed eyes keep the first acknowledgment bounded before mic startup.
    if(!ui::listening_style::begin(serviceMicCaptureEarly))Serial.println("[agent-motion] border allocation failed");
    ui::listening_style::clearBase();
}
void drawListening(AgentState& s,uint32_t now){
    if(kind!=Kind::Listening)return;
    if(lastFrame && int32_t(now-lastFrame)<16)return;
    lastFrame=now;uint32_t us=micros();
    auto f=ui::agent_motion::homeListen(ageAt(now));
    uint8_t level=services::agent_voice::waveform().level();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    if(s.recordingLevelOverride>=0)level=s.recordingLevelOverride;
#endif
    s.recordingEnvelope.advance(level,now);
    const bool capturing=services::agent_voice::isCapturing();
    hal::display::fillScreen(0xffff);
    const unsigned age=ageAt(now);
    if(listeningFromReply && age<420){
        const float progress=ui::agent_motion::ramp(age,0,420);
        restore(1-ui::agent_motion::ramp(age,0,180),78,228,22,258);
        f.avatar=1-progress;f.body=1-ui::agent_motion::ramp(age,0,160);
        actor(f,capturing);
    }else{
        if(previous){free(previous);previous=nullptr;}
        float settle=1-ui::agent_motion::ramp(age,0,280);
        ui::home_eyes::drawConversationEyes(hal::display::tft(),listeningX*settle,listeningY*settle,now);
    }
    serviceMicCaptureEarly();listening(f.listening*(listeningFromReply?ui::agent_motion::ramp(age,120,180):1),age);
    // Composite last: eye sprites contain an opaque rectangular background.
    if(ui::listening_style::ready())s.recordingBorderThickness=lroundf(ui::listening_style::paint(
        s.recordingEnvelope.level/255.f,serviceMicCaptureEarly,f.glow,true,true));
    finishFrame(us);
}
void startThinking(){thinkingSettled=false;hal::display::setFlushService(nullptr);begin(Kind::Thinking);drawThinking(millis());}
void drawThinking(uint32_t now){
    uint32_t us=micros();const unsigned age=ageAt(now);
    const bool incremental=thinkingSettled && age>=700;
    float out=1-ui::agent_motion::ramp(age,150,550);
    if(kind==Kind::Thinking && ui::listening_style::ready())hal::display::fillScreen(0xffff);
    else if(!incremental)hal::display::clearContentArea(0xffff);
    if(incremental)hal::display::tft().fillRect(118,202,44,18,0xffff);
    const float settle=ui::agent_motion::ramp(age,0,350);
    if(releaseAvatar>0 && age<350){
        ui::agent_motion::Frame f;f.avatar=releaseAvatar*(1-settle);f.body=releaseBody*(1-settle);
        actor(f);
    }else ui::home_eyes::drawThinkingEyes(hal::display::tft(),settle,now,incremental);
    listening(releaseListening*(1-ui::agent_motion::ramp(age,0,180)),age);
    const float alpha=ui::agent_motion::ramp(age,180,220);
    // Fractional row coverage keeps a 2 px bob smooth without changing dot size.
    for(int i=0;i<3;++i){
        const float y=208+sinf(age*.004f-i*1.2f)*2;
        const int top=(int)floorf(y);const float fraction=y-top;
        for(int row=0;row<6;++row){
            const float coverage=row==0?1-fraction:row==5?fraction:1;
            if(coverage>0)hal::display::tft().fillRect(122+i*14,top+row,5,1,fade(0x18c3,alpha*coverage));
        }
    }
    if(kind==Kind::Thinking && ui::listening_style::ready())
        ui::listening_style::paint(0,nullptr,releaseGlow*out,true,true);
    if(age>=700){ui::listening_style::end();thinkingSettled=true;}
    if(incremental){dirtyTop=202;dirtyBottom=220;}
    finishFrame(us,!incremental);
}
void startReply(){
    ui::listening_style::end();free(previous);previous=nullptr;
    pose=ui::eye_rig::kPresets[0].pose;begin(Kind::Reply);
}
void completeReply(AgentState& s){
    if(kind!=Kind::Reply)return;
    cancel();render::paintReplyShell(s);s.twLastMs=millis();
}
bool tick(AgentState& s,uint32_t now){
    // Global wake choreography supersedes a root-entry transition.
    if ((kind==Kind::MenuHome || kind==Kind::ReplyHome) &&
        !strcmp(ui::kami_motion::currentName(now),"wake")) { cancel(); return false; }
    if(kind==Kind::None)return false;
    if(kind==Kind::Listening){drawListening(s,now);return true;}
    if(lastFrame && int32_t(now-lastFrame)<16)return true;
    lastFrame=now;uint32_t us=micros();unsigned age=ageAt(now);
    if(kind==Kind::Thinking){drawThinking(now);return true;}
    const bool eyesOnly=(kind==Kind::ReplyHome && age>120)||(kind==Kind::MenuHome && age>300);
    if(eyesOnly){
        const auto next=kind==Kind::ReplyHome?ui::agent_motion::replyHome(age):ui::agent_motion::menuHome(age);
        const float cy=127-75*next.avatar+next.offsetY;
        const float half=(88+(16.f/2.2f-88)*next.avatar)*91.f/134.f;
        int top=(int)floorf(cy-half)-5,bottom=(int)ceilf(cy+half)+5;
        if(next.body>0){top=std::min(top,40);bottom=std::max(bottom,66);}
        dirtyTop=std::min(top,motionTop);dirtyBottom=std::max(bottom,motionBottom);
        motionTop=top;motionBottom=bottom;
        if(contentTrimmed)hal::display::tft().fillRect(14,dirtyTop,252,dirtyBottom-dirtyTop,0xffff);
        else hal::display::clearContentArea(0xffff);
    }else hal::display::clearContentArea(0xffff);
    contentTrimmed=eyesOnly;
    if(kind==Kind::Reply){
        ui::agent_motion::Frame f;
        f.avatar=ui::agent_motion::ramp(age,0,380);
        pose=ui::eye_rig::interpolate(ui::eye_rig::kPresets[0].pose,emotion::snapshot(0).pose,f.avatar);
        f.body=ui::agent_motion::ramp(age,280,140);
        float amount=ui::agent_motion::ramp(age,380,180);
        Ink ink{amount};if(amount>0)ui::pixel_dialogue::draw(ink,{22,78,236,mix(4,150,amount)},kUiDialogueBorder,kUiDialogueFill);
        actor(f);
        if(age>=560){cancel();render::paintReplyShell(s);s.twLastMs=now;}
    } else if(kind==Kind::CenteredHome){
        restore(1-ui::agent_motion::ramp(age,0,220),kSystemStatusBarH);
        actor(ui::agent_motion::Frame{});
        if(age>=280){status();cancel();render::paintEyesHomeStatic(s);}
    } else {
        auto f=kind==Kind::MenuHome?ui::agent_motion::menuHome(age):ui::agent_motion::replyHome(age);
        restore(kind==Kind::MenuHome?f.menu:f.bubble,kind==Kind::MenuHome?kSystemStatusBarH:78);
        actor(f);
        unsigned end=kind==Kind::MenuHome?600:ui::agent_motion::kReplyHomeDurationMs;
        if(age>=end){status();cancel();render::paintEyesHomeStatic(s);}
    }
    finishFrame(us,!eyesOnly);return true;
}
}}}
