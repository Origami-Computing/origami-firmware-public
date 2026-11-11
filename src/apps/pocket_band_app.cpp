#ifdef MENU_APP
#include "apps/pocket_band_app.h"
#include "services/pocket_band.h"
#include "scene/screen.h"
#include "ui/pocket_game_view.h"
#include <stdio.h>
namespace apps { namespace pocket_band {
namespace {
namespace band=services::pocket_band;
int cursor=0;bool available=false;
void enter(){cursor=0;available=band::open();}
void close(){band::close();available=false;}
void cover(){band::stop();}
void tick(uint32_t){if(available)band::poll();}
void input(hal::input::InputEvent ev){using E=hal::input::EventType;
 if(!available){if(ev.type==E::ButtonC_Tap)available=band::open();return;}
 auto s=band::snapshot();
 if(ev.type==E::Scroll)cursor=((cursor+ev.delta)%65+65)%65;
 else if(ev.type==E::ButtonC_Tap){if(cursor==64)band::tempo(s.bpm>=160?80:s.bpm+20);else band::toggle(cursor/16,cursor%16);}
 else if(ev.type==E::ButtonB_Tap){if(s.state==band::State::Playing||s.state==band::State::Rendering||s.state==band::State::Recording)band::stop();else band::play();}
 else if(ev.type==E::ButtonA_Tap&&cursor<64)band::record(cursor/16);
}
void build(ui::scene::Scene& s,int16_t,int16_t){using namespace ui::pocket;frame(s,"POCKET BAND","C toggle / B play / A record");
 if(!available){label(s,"error",{8,96,264,48},"Audio unavailable / C retry");return;}
 auto state=band::snapshot();int page=cursor<64?(cursor%16)/8:0;
 const char* names[]={"1","2","3","4"};
 for(int t=0;t<4;++t){char id[16];snprintf(id,sizeof(id),"track%d",t);label(s,id,{4,int16_t(76+t*28),20,24},names[t]);
  for(int step=0;step<8;++step){snprintf(id,sizeof(id),"cell%d",t*8+step);bool on=state.pattern[t]&(1u<<(step+page*8));s.addRect(id,{int16_t(28+step*30),int16_t(76+t*28),26,24},on?accent:0xDED7,4,1);}}
 if(cursor<64)s.addRect("selected",{int16_t(34+(cursor%8)*30),int16_t(84+(cursor/16)*28),14,8},0xFFFF,2,2);
 char status[64];snprintf(status,sizeof(status),"%u BPM / steps %d-%d",state.bpm,page*8+1,page*8+8);
 if(state.state==band::State::Recording)snprintf(status,sizeof(status),"Recording %u%% / B cancel",state.progress);
 if(state.state==band::State::Rendering)snprintf(status,sizeof(status),"Rendering %u%%",state.progress);
 if(state.state==band::State::Playing)snprintf(status,sizeof(status),"Playing / B stop");
 label(s,"status",{8,192,264,24},state.state==band::State::Error?state.error:cursor==64?"C change tempo":status);
}
void draw(TFT_eSprite& c){auto& s=ui::scene::scratchScene();build(s,280,240);ui::pocket::render(s,c);}
const ::scene::Screen screen={"Pocket Band",nullptr,nullptr,tick,input,nullptr,draw,"sequencer",build,&ui::pocket::paper};const ::scene::Screen* const screens[]={&screen};
}
App app={"Pocket Band",&screen,nullptr,nullptr,close,"pocket-band","src/apps/pocket_band_app.cpp",screens,1,nullptr,nullptr,enter,nullptr,nullptr,cover};
}}
#endif
