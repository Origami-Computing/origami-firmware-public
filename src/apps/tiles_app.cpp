#ifdef MENU_APP
#include "apps/tiles_app.h"
#include "scene/screen.h"
#include "ui/pocket_game_view.h"
#include "games/tile_merge.h"
#include <stdio.h>
namespace apps { namespace tiles {
namespace {
games::tiles::Board board;int direction=0;
void enter(){board={};direction=0;board.spawn(esp_random());board.spawn(esp_random());}
void input(hal::input::InputEvent ev){using E=hal::input::EventType;if(ev.type==E::Scroll)direction=((direction+ev.delta)%4+4)%4;else if(ev.type==E::ButtonC_Tap){if(!board.playable()){enter();return;}if(board.move(direction))board.spawn(esp_random());}}
void build(ui::scene::Scene& s,int16_t,int16_t){using namespace ui::pocket;frame(s,"2048","Wheel direction / C slide");
 for(int i=0;i<16;++i){char id[12],text[12];snprintf(id,sizeof(id),"tile%d",i);uint32_t value=1u<<board.cells[i];if(!board.cells[i])text[0]=0;else if(value>=16384)snprintf(text,sizeof(text),"%luk",(unsigned long)(value/1024));else snprintf(text,sizeof(text),"%lu",(unsigned long)value);
 uint16_t bg=board.cells[i]?uint16_t(0xEEB3-board.cells[i]*0x0820):0xDED7;
 int16_t x=76+(i%4)*32,y=68+(i/4)*32;s.addRect(id,{x,y,28,28},bg,4,1);snprintf(id,sizeof(id),"value%d",i);label(s,id,{x,y,28,28},text,ink,bg,value<1024?2:1);}
 char status[40];const char* directions[]={"LEFT","UP","RIGHT","DOWN"};snprintf(status,sizeof(status),"%s   %lu pts",directions[direction],(unsigned long)board.score);label(s,"state",{8,200,264,16},board.playable()?status:"No moves / C new game");}
void draw(TFT_eSprite& c){auto& s=ui::scene::scratchScene();build(s,280,240);ui::pocket::render(s,c);}
const ::scene::Screen screen={"2048",nullptr,nullptr,nullptr,input,nullptr,draw,"board",build,&ui::pocket::paper};
const ::scene::Screen* const screens[]={&screen};
}
App app=SCENE_APP_CATALOG_LIFECYCLE("2048","2048",&screen,enter,nullptr,"src/apps/tiles_app.cpp",screens);
}}
#endif
