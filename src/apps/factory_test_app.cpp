#include "apps/factory_test_app.h"
#include "apps/boot_app.h"
#include "services/factory_test.h"
#include "services/manufacturing.h"
#include "services/manufacturing_access.h"
#include "hal/hal_power.h"
#include "core/release_policy.h"
#include "apps/camera_menu_app.h"
#include "services/onboarding_progress.h"
#include "services/vibration.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "sfx/sfx_lib.h"
#include "ui/recorder_view.h"
#include <Arduino.h>
namespace apps { namespace factory_test {
namespace {
using services::factory_test::Check;
using hal::input::EventType;
constexpr unsigned count=services::factory_test::kCount;
constexpr uint16_t bg=0x0841,fg=0xffff,green=0x67ee,red=0xfaaa,muted=0xbdf7;
services::factory_test::Session session;
uint32_t passed=0,failed=0,stepAt=0,lastDraw=0;
unsigned step=0,selection=0,buttons=0;
bool bootChecked=false,pending=false,summary=false,saveFailed=false;
uint32_t backHeld=0;bool suppressBackTap=false;
uint32_t recorded=0;
const char* names[]={"A","B","C","turn left","turn right","A+B"};
bool manual(){return step==unsigned(Check::Display)||step==unsigned(Check::Speaker)||step==unsigned(Check::Vibration)||step==unsigned(Check::CameraQuality)||step==unsigned(Check::AudioQuality);}
void cleanup(){session.close();services::vibration::stopPreview();}
void begin(unsigned next){
    cleanup();step=next;buttons=0;stepAt=millis();pending=true;summary=false;saveFailed=false;
    passed&=~(1u<<step);failed&=~(1u<<step);recorded&=~(1u<<step);
    saveFailed=!services::manufacturing::invalidate(step);
}
void logReport(){
    Serial.printf("FACTORY STATUS active=%u step=%u passed=%03lx failed=%03lx complete=%u\n",app.currentScreen!=nullptr,step,(unsigned long)passed,(unsigned long)failed,services::factory_test::completed());
    Serial.printf("FACTORY DETAIL summary=%u pending=%u buttons=%u detail=%s\n",summary,pending,buttons,session.detail());
}
void results(){
    cleanup();summary=true;pending=false;selection=0;
    saveFailed=!services::factory_test::saveReport(passed,failed,false);logReport();
}
void visit(unsigned next){
    cleanup();step=next;summary=false;pending=false;saveFailed=false;
    if(!((passed|failed)&(1u<<step)))begin(step);
}
void advance(){if(step+1<count)visit(step+1);else results();}
bool active(){return app.currentScreen!=nullptr;}
void enter(){passed=failed=0;
    if(!services::manufacturing::report(passed,failed))services::manufacturing::begin();
    recorded=passed|failed;summary=true;pending=false;selection=0;}
void resume(){if(!summary && step!=unsigned(Check::CameraQuality))visit(step);}
void exit(){cleanup();}
void tick(uint32_t now){
    if(summary||((passed|failed)&recorded&(1u<<step)))return;
    if(pending){
        if(now-stepAt<200)return;pending=false;stepAt=now;
        if(step==unsigned(Check::Speaker)){
            const int volume=sfx::volume();sfx::setVolume(60);sfx::play(sfx::kSfxAlarmChimePreview);sfx::setVolume(volume);
        }else if(step==unsigned(Check::Vibration))services::vibration::preview(1,3000);
        else if(step==unsigned(Check::CameraQuality)){cleanup();NavHandler::instance().push(&apps::camera_menu::app);}
        else if(step!=unsigned(Check::Buttons)&&(!manual()||step==unsigned(Check::AudioQuality)))session.start(static_cast<Check>(step),now);
        // Hardware startup may block and reset its measurement start time.
        // Refresh the frame timestamp before evaluating that new session.
        now=millis();
    }
    if(step==unsigned(Check::Buttons)){
        if(buttons==63)passed|=1u<<step;
        else if(now-stepAt>45000)failed|=1u<<step;
    }else if(!manual()||step==unsigned(Check::AudioQuality)){
        session.tick(now);
        if(session.passed())passed|=1u<<step;
        if(session.failed())failed|=1u<<step;
    }
    const uint32_t bit=1u<<step;
    if(((passed|failed)&bit)&&!(recorded&bit)){saveFailed=!services::manufacturing::result(step,(passed&bit)!=0);if(!saveFailed)recorded|=bit;}
}
void input(hal::input::InputEvent ev){
    if(ev.type==EventType::ButtonB_Press){backHeld=millis();suppressBackTap=false;}
    if(ev.type==EventType::ButtonB_Release && backHeld && millis()-backHeld>=800){results();selection=step;suppressBackTap=true;backHeld=0;return;}
    if(ev.type==EventType::ButtonB_Tap && suppressBackTap){suppressBackTap=false;return;}
    if(summary){
        if(ev.type==EventType::Scroll)selection=(selection+count+1+(ev.delta>0?1:-1))%(count+1);
        if(ev.type==EventType::ButtonA_Tap && selection<count){
            passed&=~(1u<<selection);failed|=1u<<selection;
            saveFailed=!services::manufacturing::result(selection,false);return;
        }
        if(ev.type==EventType::ButtonB_Tap && selection<count){begin(selection);return;}
        if(ev.type==EventType::ButtonC_Tap){
            if(selection<count){visit(selection);}
            else if(passed==services::factory_test::kAll && !failed){
                if(services::factory_test::saveReport(passed,failed,true)){bootChecked=true;NavHandler::instance().pop();}
                else saveFailed=true;
            }
        }
        return;
    }
    if(pending)return;
    if(step==unsigned(Check::Buttons)&& !(passed&(1u<<step)) && !(failed&(1u<<step))){
        if(ev.type==EventType::ButtonA_Tap)buttons|=1;
        if(ev.type==EventType::ButtonB_Tap)buttons|=2;
        if(ev.type==EventType::ButtonC_Tap)buttons|=4;
        if(ev.type==EventType::Scroll)buttons|=ev.delta<0?8:16;
        return;
    }
    if(ev.type==EventType::Scroll){
        int next=int(step)+(ev.delta>0?1:-1);
        if(next>=0&&next<int(count))visit(unsigned(next));else results();return;
    }
    if(step==unsigned(Check::AudioQuality) && !((passed|failed)&(1u<<step))){
        if(ev.type==EventType::ButtonA_Tap){stationAudio(true);return;}
        if(ev.type==EventType::ButtonB_Tap){stationAudio(false);return;}
    }
    if(ev.type==EventType::ButtonB_Tap){begin(step);return;}
    if(ev.type==EventType::ButtonA_Tap){failed|=1u<<step;passed&=~(1u<<step);saveFailed=!services::manufacturing::result(step,false);advance();return;}
    if(ev.type==EventType::ButtonC_Tap){
        if(manual()){
            saveFailed=!services::manufacturing::result(step,true);
            if(saveFailed)return;
            passed|=1u<<step;failed&=~(1u<<step);recorded|=1u<<step;}
        if((passed&(1u<<step))&&!saveFailed)advance();
    }
}
bool chord(){if(!summary && step==unsigned(Check::Buttons)&&!(recorded&(1u<<step)))buttons|=32;else{results();selection=step;}return true;}
void text(TFT_eSprite& c,const char* value,int x,int y,uint16_t color=fg,int size=1){
    c.setTextColor(color,bg);c.setTextSize(size);c.drawString(value,x,y,1);c.setTextSize(1);
}
void progress(TFT_eSprite& c){
    unsigned good=__builtin_popcount(passed),bad=__builtin_popcount(failed),done=good+bad;
    c.fillRect(0,0,280,52,bg);
    char line[48];snprintf(line,sizeof(line),"%u/%u complete  %u passed",done,count,good);text(c,line,16,8);
    snprintf(line,sizeof(line),"%u failed   %u waiting",bad,count-done);text(c,line,16,24,bad?red:muted);
    c.fillRect(16,40,248,4,muted);c.fillRect(16,40,248*good/count,4,green);
    c.fillRect(16+248*good/count,40,248*bad/count,4,red);
}
void draw(TFT_eSprite& c){
    if(millis()-lastDraw<100)return;lastDraw=millis();
    c.fillSprite(bg);c.setTextDatum(TL_DATUM);
    if(!summary && step==unsigned(Check::AudioQuality) && !((passed|failed)&(1u<<step))){
        char wave[28];session.audioWave(wave);
        recorder_view::render(c,session.audioPhase()==1?1:session.audioPhase()==2?2:0,session.audioSeconds(millis()),millis(),0,wave);
        progress(c);
        c.fillRect(84,112,192,24,0xffff);c.setTextColor(0x2104,0xffff);
        c.drawString(session.audioPhase()==3?"PLAYING":session.operatorReady()?"LISTENED - C: PASS":session.audioPhase()==2?"B: PLAY TAKE":session.audioPhase()==1?"RECORDING":"A: RECORD",88,116,1);
        c.fillRect(0,200,280,40,bg);
        text(c,saveFailed?"SAVE FAILED - C: retry save":session.audioPhase()==1?"A: stop recording":"A: record   B: play   C: pass",16,204,saveFailed?red:fg);
        text(c,"Rotate: prev/next  A+B: test list",16,224,muted);
        hal::display::markAllDirty();return;
    }
    progress(c);
    if(summary){
        bool all=passed==services::factory_test::kAll && !failed;
        text(c,all?"ALL CHECKS PASSED":"CHECKS NEED ATTENTION",16,56,all?green:red,2);
        unsigned first=(selection/4)*4;
        for(unsigned i=first;i<=count && i<first+4;++i){
            char row[50];
            if(i<count)snprintf(row,sizeof(row),"%c %-18s %s",selection==i?'>':' ',services::factory_test::name(static_cast<Check>(i)),passed&(1u<<i)?"PASS":failed&(1u<<i)?"FAIL":"WAIT");
            else snprintf(row,sizeof(row),"%c Continue to tutorial",selection==i?'>':' ');
            text(c,row,16,92+(i-first)*24,i==count?(all?green:muted):(passed&(1u<<i)?green:red));
        }
        text(c,saveFailed?"SAVE FAILED - retry before continuing":"Rotate: choose   C: open / finish",16,196,saveFailed?red:muted);
        text(c,"A: fail selected   B: retry selected",16,216,muted);
    }else{
        char heading[48];snprintf(heading,sizeof(heading),"%u/%u %s",step+1,count,services::factory_test::name(static_cast<Check>(step)));
        text(c,heading,16,56,fg,strlen(heading)>21?1:2);
        bool ok=passed&(1u<<step),bad=failed&(1u<<step);
        text(c,ok?"PASS":bad?"FAIL / RETRY":manual()?"OPERATOR CHECK":"IN PROGRESS",16,84,ok?green:bad?red:muted,2);
        if(step==unsigned(Check::Buttons)){
            for(unsigned i=0;i<6;++i){char row[30];snprintf(row,sizeof(row),"[%c] %s",buttons&(1u<<i)?'x':' ',names[i]);text(c,row,16+(i%2)*128,120+(i/2)*22,buttons&(1u<<i)?green:fg);}
        }else if(step==unsigned(Check::Display)){
            const uint16_t colors[]={0xf800,0x07e0,0x001f,0xffff,0x0000};
            for(int i=0;i<5;++i){c.fillRect(16+i*50,112,46,52,colors[i]);c.drawRect(16+i*50,112,46,52,muted);}
            text(c,"Check colors, lines, dead pixels",16,176);
        }else{
            const char* help=step==unsigned(Check::Speaker)?"Unplug headphones. Hear the chime?":step==unsigned(Check::Vibration)?"Feel the vibration motor?":step==unsigned(Check::Motion)?"Tilt and rotate all axes":step==unsigned(Check::Microphone)?"Speak near the mic for 3 seconds":"Hardware measurement";
            text(c,help,16,116);
            c.setTextColor(muted,bg);c.setTextSize(1);c.setCursor(16,144);c.setTextWrap(true);c.print(pending?"Starting...":manual()?"C confirms only if it worked.":session.detail());
        }
        text(c,"A: fail  B: retry  C: pass/next",16,196,fg);
        text(c,step==unsigned(Check::Buttons)&&!ok&&!bad?"Hold B: test list (45s timeout)":"Rotate: prev/next  A+B: test list",16,216,muted);
    }
    hal::display::markAllDirty();
}
const ::scene::Screen screen={"Hardware test",nullptr,cleanup,tick,input,chord,draw,"factory-test",nullptr};
const ::scene::Screen* const screens[]={&screen};
AppPresentation presentation(){return {true,true,AppPresentation::StatusBar::Hidden};}
core::health::RuntimePolicy policy(){core::health::RuntimePolicy p{false,false,false,true,false};p.allowBackgroundUploads=false;p.allowAutomaticUpdate=false;return p;}
}
App app=SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Hardware test","factory-test",&screen,enter,resume,exit,"src/apps/factory_test_app.cpp",screens,presentation,policy);
bool requestServiceAccess(){
    auto& nav=NavHandler::instance();
    if(!nav.root()||nav.root()==&apps::boot::app||nav.systemScreenActive())return false;
    return services::manufacturing_access::session().request(millis(),hal::power::externalPowerPresent(hal::power::read()));
}
bool serviceBootOffer(){
    if(active())return true;
    if(bootChecked)return false;
    auto& nav=NavHandler::instance();
    if(nav.root()==&apps::boot::app||nav.depth()!=1||nav.systemScreenActive())return true;
    bootChecked=true;
    // Once onboarding starts, its unfinished checkpoint owns boot recovery.
    const auto onboarding=services::onboarding_progress::load();
    if(core::release_policy::productionOnly||services::manufacturing::accepted()||onboarding.offered||onboarding.completed)return false;
    nav.push(&app);return true;
}
void restart(){NavHandler::instance().popToRoot();bootChecked=true;
    services::manufacturing::begin();services::factory_test::saveReport(0,0,false);
    NavHandler::instance().push(&app);passed=failed=recorded=0;begin(0);}
void printStatus(){logReport();services::manufacturing::printStatus();}
bool stationRun(unsigned check){
    if(check>=count)return false;
    if(NavHandler::instance().top()!=&app){NavHandler::instance().popToRoot();bootChecked=true;NavHandler::instance().push(&app);}
    begin(check);return true;
}
bool stationRecord(unsigned check,bool ok){
    if(check>=count)return false;
    // Explicit operator judgment, independent of a running measurement.
    // Stop the active session so a later tick cannot overwrite this judgment.
    NavHandler::instance().popToRoot();
    if(!services::manufacturing::result(check,ok))return false;
    services::manufacturing::report(passed,failed);recorded=passed|failed;
    return true;
}
bool stationConfirm(bool ok){
    if(!active()||summary||pending||millis()-stepAt<1000)return false;
    // Only sensory checks can be confirmed by an operator; buttons, motion and
    // measurements must pass through their actual production test owner.
    if(!manual()||(step==unsigned(Check::AudioQuality)&&!session.operatorReady()))return false;
    if(step==unsigned(Check::CameraQuality)){uint32_t p=0,f=0;if(!services::manufacturing::report(p,f)||!(p&(1u<<unsigned(Check::Camera))))return false;}
    passed&=~(1u<<step);failed&=~(1u<<step);if(ok)passed|=1u<<step;else failed|=1u<<step;
    if(!services::manufacturing::result(step,ok))return false;
    results();return true;
}
} }

namespace apps { namespace factory_test {
bool stationAudio(bool record){
    if(!active()||summary||pending||step!=unsigned(Check::AudioQuality))return false;
    if(record && session.audioPhase()!=1){
        passed&=~(1u<<step);failed&=~(1u<<step);recorded&=~(1u<<step);
        if(!services::manufacturing::invalidate(step))return false;
    }
    return record?session.audioRecord(millis()):session.audioPlay();
}
} }
