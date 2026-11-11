#ifdef MENU_APP
#include "apps/minesweeper_app.h"
#include "scene/screen.h"
#include "ui/pocket_game_view.h"
#include "games/pocket_mines.h"
#include <stdio.h>
namespace apps { namespace minesweeper {
namespace {
games::mines::Board board;int cursor=12;
void enter(){board={};cursor=12;}
void input(hal::input::InputEvent ev){using E=hal::input::EventType;if(ev.type==E::Scroll)cursor=((cursor+ev.delta)%25+25)%25;else if(ev.type==E::ButtonC_Tap){if(board.lost||board.won())enter();else board.reveal(cursor,esp_random());}else if(ev.type==E::ButtonB_Tap)board.toggle(cursor);}
void build(ui::scene::Scene& s,int16_t,int16_t){using namespace ui::pocket;frame(s,"MINESWEEPER","Wheel / C reveal / B flag");int flags=0;
 s.addRect("cursor",{int16_t(70+cursor%5*28),int16_t(66+cursor/5*28),28,28},accent,4,1);
 for(int i=0;i<25;++i){char id[12],text[4]="";snprintf(id,sizeof(id),"cell%d",i);if(board.lost&&board.bomb[i])text[0]='*';else if(board.flag[i]){text[0]='F';++flags;}else if(board.open[i]){int n=board.neighbors(i);if(n)text[0]='0'+n;}else text[0]='.';uint16_t bg=board.open[i]?paper:0xDED7;label(s,id,{int16_t(72+i%5*28),int16_t(68+i/5*28),24,24},text,ink,bg,2);}
 char info[32];snprintf(info,sizeof(info),"5 mines / %d flags",flags);label(s,"state",{8,204,264,16},board.lost?"Mine! C new game":board.won()?"All clear! C new game":info);}
void draw(TFT_eSprite& c){auto& s=ui::scene::scratchScene();build(s,280,240);ui::pocket::render(s,c);}
const ::scene::Screen screen={"Minesweeper",nullptr,nullptr,nullptr,input,nullptr,draw,"board",build,&ui::pocket::paper};
const ::scene::Screen* const screens[]={&screen};
}
App app=SCENE_APP_CATALOG_LIFECYCLE("Minesweeper","minesweeper",&screen,enter,nullptr,"src/apps/minesweeper_app.cpp",screens);
}}
#endif
