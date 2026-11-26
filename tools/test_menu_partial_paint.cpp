// Banded menu repaint: the panel, updated only with the rows each draw
// returns, must always equal a full-frame paint of the same pose. Covers
// highlight moves, interrupted moves, scrolling, the label marquee and the
// focused icon's animation frames.
// c++ -std=c++17 -fsanitize=address,undefined -Iinclude tools/test_menu_partial_paint.cpp
#include "ui/menu_list.h"
#include <array>
#include <cassert>
#include <cstdio>
struct Canvas {
 std::array<uint16_t,280*240> pixels{};
 void fillRect(int x,int y,int w,int h,uint16_t color){
  assert(x>=0&&y>=0&&x+w<=280&&y+h<=240);
  for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)pixels[yy*280+xx]=color;
 }
};
static Canvas canvas,panel,reference;
static uint8_t still[200],frames[2][200];
static int iconFrame=0;
static ui::ListItem row(int i){
 ui::ListItem item;item.label=i==1?"A very long menu caption with its complete ending":"Screen brightness";
 item.icon=still;item.opensScreen=i%2==0;
 if(i==0||i==3){item.icon=frames[iconFrame];item.iconColor=0xf800;}
 return item;
}
struct List{int visibleRows=0,totalItems=7,selected=0,scrollFirst=0;void clampScroll(){
 if(selected<scrollFirst)scrollFirst=selected;if(selected>=scrollFirst+visibleRows)scrollFirst=selected-visibleRows+1;}};
static ui::menu_list::State state;static List list;
static int partial=0,full=0;
static void frame(unsigned now,bool dirty=false,bool focusDirty=false){
 using namespace ui::menu_list;
 const Band band=state.draw(canvas,list,row,now,dirty,focusDirty);
 if(band){
  (band.y1-band.y0==240?full:partial)++;
  for(int y=band.y0;y<band.y1;++y)for(int x=0;x<280;++x)panel.pixels[y*280+x]=canvas.pixels[y*280+x];
 }
 paint(reference,state.painted,list.totalItems,list.selected,row,now-state.focusAt);
 for(int i=0;i<280*240;++i)if(panel.pixels[i]!=reference.pixels[i]){
  std::printf("FAIL t=%u pixel (%d,%d) band=%d-%d\n",now,i%280,i/280,band.y0,band.y1);assert(false);}
}
int main(){
 for(int i=0;i<200;++i){still[i]=uint8_t(i*37);frames[0][i]=uint8_t(i*11);frames[1][i]=uint8_t(255-i*13);}
 unsigned t=0;frame(t,true);
 auto run=[&](unsigned ms){for(unsigned end=t+ms;t<end;){t+=16;frame(t);}};
 list.selected=1;list.clampScroll();run(400);                       // down; marquee on row 1
 list.selected=2;run(80);list.selected=0;run(400);                  // interrupted, then back up
 for(int s=1;s<7;++s){list.selected=s;list.clampScroll();run(120);} // scrolls the window
 list.selected=3;list.clampScroll();run(300);
 for(int k=0;k<4;++k){iconFrame^=1;t+=75;frame(t,false,true);}     // focused icon animation
 run(3000);                                                         // dwell, marquee scroll and return
 assert(partial>full&&full>0);
 // The highlight moving between two visible rows never transfers the whole frame.
 const int before=full;list.selected=list.selected==list.scrollFirst?list.selected+1:list.selected-1;run(400);assert(full==before);
 std::printf("PASS banded menu repaint matches full-frame paint (%d partial, %d full transfers)\n",partial,full);
}
