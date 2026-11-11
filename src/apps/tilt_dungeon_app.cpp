#ifdef MENU_APP
#include "apps/tilt_dungeon_app.h"
#include "games/tilt_dungeon.h"
#include "services/motion.h"
#include "scene/screen.h"
#include "ui/pocket_game_view.h"
#include <stdio.h>
namespace apps { namespace tilt_dungeon {
namespace {
games::dungeon::World world;services::motion::GyroFifo motion;
uint32_t seed=1,last=0;int32_t neutral[3]={};bool paused=true,ready=false,calibrated=false,failed=false;
constexpr uint16_t bg=0x10E4,stone=0x32A9,gold=0xF609,mint=0x97B3;
void release(){motion.close();ready=false;paused=true;calibrated=false;}
void enter(){release();failed=false;seed=esp_random()%1000000;world.generate(seed);last=millis();}
void input(hal::input::InputEvent e){using E=hal::input::EventType;
 if(e.type==E::ButtonC_Tap){
  if(world.won){world.generate(++seed);return;}
  if(!paused){release();return;}
  ready=motion.open();failed=!ready;paused=!ready;calibrated=false;last=millis();
 }else if(e.type==E::Scroll&&paused){seed=uint32_t((int32_t(seed)+e.delta+1000000)%1000000);world.generate(seed);}
}
void tick(uint32_t now){uint32_t dt=now-last;last=now;if(paused)return;
 motion.poll();int32_t acceleration[3];if(!motion.readAcceleration(acceleration)){release();failed=true;return;}
 if(!calibrated){for(int i=0;i<3;++i)neutral[i]=acceleration[i];calibrated=true;return;}
 if(dt>100)dt=100;while(dt){uint32_t step=dt>8?8:dt;world.step((acceleration[0]-neutral[0])/400.f,(acceleration[1]-neutral[1])/400.f,step/1000.f);dt-=step;}
 if(world.won)release();
}
void build(ui::scene::Scene& s,int16_t,int16_t){using A=ui::scene::TextAlign;s.clear();s.addRect("bg",{0,0,280,240},bg);
 s.addLabel("title",{8,40,264,24},"TILT DUNGEON",mint,bg,2,A::MiddleCenter,1);
 int count=0;for(int y=0;y<7;++y)for(int x=0;x<11;){if(!world.wall[y*11+x]){++x;continue;}int start=x;while(x<11&&world.wall[y*11+x])++x;char id[16];snprintf(id,sizeof(id),"wall%d",count++);s.addRect(id,{int16_t(8+start*24),int16_t(64+y*20),int16_t((x-start)*24),20},stone,2,1);}
 for(int i=0;i<77;++i)if(world.gem[i]){char id[16];snprintf(id,sizeof(id),"gem%d",i);s.addRect(id,{int16_t(16+(i%11)*24),int16_t(70+(i/11)*20),8,8},gold,3,2);}
 s.addRect("exit",{228,168,16,12},world.collected==3?mint:stone,4,2);
 s.addRect("ball",{int16_t(8+world.x*24-4),int16_t(64+world.y*20-4),8,8},0xFFFF,4,3);
 char info[64];snprintf(info,sizeof(info),"%06lu / %d of 3 gems",(unsigned long)seed,world.collected);
 s.addLabel("score",{8,204,264,16},info,gold,bg,1,A::MiddleCenter,2);
 s.addLabel("hint",{4,222,272,16},world.won?"Escaped! C new dungeon":failed?"Motion unavailable / C retry":paused?"Hold level / C start / Wheel seed":"Tilt to move / C pause",mint,bg,1,A::MiddleCenter,2);
}
void draw(TFT_eSprite& c){auto& s=ui::scene::scratchScene();build(s,280,240);ui::pocket::render(s,c);}
const ::scene::Screen screen={"Tilt Dungeon",nullptr,nullptr,tick,input,nullptr,draw,"dungeon",build,&bg};const ::scene::Screen* const screens[]={&screen};
}
App app={"Tilt Dungeon",&screen,nullptr,nullptr,release,"tilt-dungeon","src/apps/tilt_dungeon_app.cpp",screens,1,nullptr,nullptr,enter,nullptr,nullptr,release};
}}
#endif
