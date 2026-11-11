#ifdef MENU_APP
#include "apps/breakout_app.h"
#include "scene/screen.h"
#include "ui/pocket_game_view.h"
#include "games/pocket_breakout.h"
#include <stdio.h>
namespace apps { namespace breakout {
namespace {
games::breakout::Game game;uint32_t last=0;
void enter(){game.reset();last=millis();}
void pause(){game.running=false;last=millis();}
void tick(uint32_t now){uint32_t dt=now-last;last=now;if(dt>250)dt=250;while(dt){uint32_t step=dt>8?8:dt;game.step(step/1000.f);dt-=step;}}
void input(hal::input::InputEvent ev){using E=hal::input::EventType;if(ev.type==E::Scroll)game.wheel(ev.delta);else if(ev.type==E::ButtonC_Tap){if(game.won||!game.lives)enter();else {game.running=!game.running;last=millis();}}}
void build(ui::scene::Scene& s,int16_t,int16_t){using namespace ui::pocket;char title[32];snprintf(title,sizeof(title),"BREAKOUT  %d/24",game.score);frame(s,title,game.won?"Cleared! C play again":!game.lives?"Game over / C again":game.running?"Wheel paddle / C pause":"C launch / Wheel paddle");
 const uint16_t colors[]={0xCB46,0xE489,0xE5AC,0x6C91};for(int i=0;i<24;++i)if(game.bricks[i]){char id[12];snprintf(id,sizeof(id),"brick%d",i);s.addRect(id,{int16_t(12+i%6*44),int16_t(76+i/6*20),40,16},colors[i/6],3,1);}
 s.addRect("paddle",{int16_t(game.paddle),200,48,6},ink,3,2);s.addRect("ball",{int16_t(game.x-3),int16_t(game.y-3),6,6},ink,3,2);
 char lives[16];snprintf(lives,sizeof(lives),"%d lives",game.lives);label(s,"lives",{8,60,264,16},lives);}
void draw(TFT_eSprite& c){auto& s=ui::scene::scratchScene();build(s,280,240);ui::pocket::render(s,c);}
const ::scene::Screen screen={"Breakout",nullptr,nullptr,tick,input,nullptr,draw,"court",build,&ui::pocket::paper};
const ::scene::Screen* const screens[]={&screen};
}
App app={"Breakout",&screen,nullptr,nullptr,pause,"breakout","src/apps/breakout_app.cpp",screens,1,nullptr,nullptr,enter,nullptr,nullptr,pause};
}}
#endif
