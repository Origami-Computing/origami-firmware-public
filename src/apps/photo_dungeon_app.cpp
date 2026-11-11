#ifdef MENU_APP
#include "apps/photo_dungeon_app.h"
#include "games/tilt_dungeon.h"
#include "services/camera_grid.h"
#include "scene/screen.h"
#include "ui/pocket_game_view.h"
#include <stdio.h>
namespace apps { namespace photo_dungeon {
namespace {
games::dungeon::World world;services::camera_grid::Image photo{};int direction=0;bool havePhoto=false;
constexpr uint16_t bg=0x10E4,stone=0x32A9,gold=0xF609,mint=0x97B3;
void release(){services::camera_grid::close();}
void enter(){release();havePhoto=false;direction=0;world.generate(1);}
void input(hal::input::InputEvent e){using E=hal::input::EventType;using namespace services::camera_grid;
 if(state()==State::Waiting||state()==State::Decoding)return;
 if(e.type==E::ButtonA_Tap||(!havePhoto&&e.type==E::ButtonC_Tap)){begin();return;}
 if(!havePhoto)return;
 if(e.type==E::Scroll)direction=((direction+e.delta)%4+4)%4;
 if(e.type==E::ButtonC_Tap){if(world.won){world.fromPhoto(photo.luminance);return;}const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};for(int i=0;i<10;++i)world.step(dx[direction],dy[direction],1.f/24);}
}
void tick(uint32_t){using namespace services::camera_grid;poll();if(state()==State::Ready&&copy(photo)){world.fromPhoto(photo.luminance);havePhoto=true;close();}}
void build(ui::scene::Scene& s,int16_t,int16_t){using A=ui::scene::TextAlign;s.clear();s.addRect("bg",{0,0,280,240},bg);
 s.addLabel("title",{8,40,264,24},"PHOTO DUNGEON",mint,bg,2,A::MiddleCenter,1);
 if(!havePhoto){using namespace services::camera_grid;const char* hint=state()==State::Waiting||state()==State::Decoding?"Taking photo...":state()==State::Failed?error():"Aim camera / C take photo";s.addLabel("prompt",{8,96,264,64},hint,mint,bg,1,A::MiddleCenter,1);return;}
 int count=0;for(int y=0;y<7;++y)for(int x=0;x<11;){if(!world.wall[y*11+x]){++x;continue;}int start=x;while(x<11&&world.wall[y*11+x])++x;char id[16];snprintf(id,sizeof(id),"wall%d",count++);s.addRect(id,{int16_t(8+start*24),int16_t(64+y*20),int16_t((x-start)*24),20},stone,2,1);}
 for(int i=0;i<77;++i)if(world.gem[i]){char id[16];snprintf(id,sizeof(id),"gem%d",i);s.addRect(id,{int16_t(16+(i%11)*24),int16_t(70+(i/11)*20),8,8},gold,3,2);}
 s.addRect("exit",{228,168,16,12},world.collected==3?mint:stone,4,2);
 s.addRect("ball",{int16_t(8+world.x*24-4),int16_t(64+world.y*20-4),8,8},0xFFFF,4,3);
 const char* directions[]={"UP","RIGHT","DOWN","LEFT"};
 char info[64];snprintf(info,sizeof(info),"%s / %d of 3 gems",directions[direction],world.collected);
 s.addLabel("score",{8,204,264,16},info,gold,bg,1,A::MiddleCenter,2);
 using namespace services::camera_grid;
 const char* hint=state()==State::Waiting||state()==State::Decoding?"Taking photo...":state()==State::Failed?error():!havePhoto?"Aim camera / C take photo":world.won?"Escaped! C replay / A new photo":"Wheel direction / C step / A photo";
 s.addLabel("hint",{4,222,272,16},hint,mint,bg,1,A::MiddleCenter,2);
}
void draw(TFT_eSprite& c){auto& s=ui::scene::scratchScene();build(s,280,240);ui::pocket::render(s,c);}
const ::scene::Screen screen={"Photo Dungeon",nullptr,nullptr,tick,input,nullptr,draw,"dungeon",build,&bg};const ::scene::Screen* const screens[]={&screen};
}
App app={"Photo Dungeon",&screen,nullptr,nullptr,release,"photo-dungeon","src/apps/photo_dungeon_app.cpp",screens,1,nullptr,nullptr,enter,nullptr,nullptr,release};
}}
#endif
