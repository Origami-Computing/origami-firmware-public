#include "ui/menu_list.h"
#include <cassert>
#include <cstdio>
#include <array>
struct Canvas {
 std::array<uint16_t,280*240> pixels{};
 void fillRect(int x,int y,int w,int h,uint16_t color){
  assert(x>=0&&y>=0&&x+w<=280&&y+h<=240);
  for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)pixels[yy*280+xx]=color;
 }
};
static Canvas a,b,reference;
static uint8_t icon[200]={0xff};
static ui::ListItem row(int i){ui::ListItem item;item.icon=icon;item.label=i==0?"A very long menu caption with its complete ending":"Screen brightness";return item;}
int main(){
 using namespace ui::menu_list;
 for(const char* text:{"Connect to app","Update available","Screen brightness"}){
  auto item=row(0);item.label=text;
  assert(ui::proportional::width(text,fontSize)<=textRight-textX(item));
  assert(textOffset(item,9000)==0);
 }
 const Pose pose{16,66,0};paint(a,pose,2,0,row,0);paint(b,pose,2,0,row,2400);
 bool changed=false;
 for(int y=0;y<240;++y)for(int x=0;x<280;++x){
  if(x>=textLeft&&x<textRight&&y>=16&&y<66)changed|=a.pixels[y*280+x]!=b.pixels[y*280+x];
  else assert(a.pixels[y*280+x]==b.pixels[y*280+x]);
 }
 assert(changed); // Scroll only selected text, never icons/arrows/other rows.
 reference.fillRect(0,0,280,240,0xffff);
 ui::proportional::atBaseline(reference,"Screen brightness",textLeft,
   ui::proportional::rowBaseline(top+pitch,rowHeight,20),20,0,0xffff);
 for(int y=top+pitch;y<top+pitch+rowHeight;++y)for(int x=textLeft;x<textRight;++x)
  assert(a.pixels[y*280+x]==reference.pixels[y*280+x]);
 assert(textOffset(row(0),1599)==0&&textOffset(row(0),1640)==1);
 const int overflow=ui::proportional::width(row(0).label,fontSize)-(textRight-textLeft);
 assert(textOffset(row(0),1600+overflow*40)==overflow);
 assert(textOffset(row(0),3200+overflow*80)==0);
 struct List{int visibleRows=0,totalItems=2,selected=0,scrollFirst=0;void clampScroll(){}} list;
 State state;state.draw(a,list,row,0,true);
 for(unsigned ms=16;ms<=2400;ms+=16)state.draw(a,list,row,ms,false);
 assert(state.lastTextOffset>0&&list.visibleRows==4);
 list.selected=1;state.draw(a,list,row,2416,false);assert(state.lastTextOffset==0);
 list.selected=0;state.draw(a,list,row,2432,false);assert(state.lastTextOffset==0);
 state.draw(a,list,row,5000,false);assert(state.lastTextOffset==0); // Re-entry gets a fresh dwell.
 puts("PASS fixed 20px font, label fit, row baseline, clipped marquee, dwell/endpoints and focus reset");
}
