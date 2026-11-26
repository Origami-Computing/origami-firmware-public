#pragma once
#include "ui/menu_motion.h"
#include "ui/focused_label.h"
#include "ui/list_row.h"
#include "ui/proportional_text.h"
#include "ui/rounded_fill.h"
#include "config/ui_theme.h"

// Shared four-row menu painter. Uses the existing surface, native icon masks,
// and ListView's input/window state; no extra framebuffer or navigation stack.
namespace ui { namespace menu_list {
struct Layout {int top=16,pitch=56,height=50,bottom=234;};
constexpr int fontSize=20, textLeft=68, textRight=246;
inline int textX(const ListItem& item){return item.icon?textLeft:32;}
inline int textOffset(const ListItem& item,unsigned elapsed){return focused_label::offset(elapsed,proportional::width(item.label?item.label:"",fontSize)-(textRight-textX(item)));}
template<class C> struct Ink {
 C& c; float y0,y1; bool highlight; bool editing; int clipLeft,clipRight; const Layout& layout;
 unsigned coverage(int x,int y)const{return highlight?rounded_fill::coverage({12,y0,256,y1-y0,float(layout.height)/2},x,y):0;}
 void fillRect(int x,int y,int w,int h,uint16_t color){
  for(int yy=std::max(layout.top,y);yy<std::min(layout.bottom,y+h);++yy)
   for(int xx=std::max(clipLeft,x);xx<std::min(clipRight,x+w);++xx)
    c.fillRect(xx,yy,1,1,editing
      ?rounded_fill::blend(kUiEdit,rounded_fill::blend(0,0xffff,coverage(xx,yy)),255-unsigned(color>>11)*255/31)
      :rounded_fill::blend(uint16_t(~color),color,coverage(xx,yy)));
 }
};
template<class C,class Items> void paint(C& c,Pose p,int count,int selected,Items itemAt,unsigned focusedMs=0,Layout layout=Layout{}){
 c.fillRect(0,0,280,240,0xffff);
 const auto selection=count?itemAt(selected):ListItem{};
 const bool highlight=count && (!selection.editing||selection.blinkOn);
 const float y0=p.top-p.scroll,y1=p.bottom-p.scroll;
 if(highlight)rounded_fill::draw(c,{12,y0,256,y1-y0,float(layout.height)/2},0,0xffff,{0,layout.top,280,layout.bottom-layout.top});
 Ink<C> ink{c,y0,y1,highlight,false,0,280,layout};
 for(int i=0;i<count;++i){
  int y=int(std::round(layout.top+i*layout.pitch-p.scroll));if(y+layout.height<layout.top||y>=layout.bottom)continue;
  const auto item=itemAt(i); const char* label=item.label?item.label:"";
  const int x=textX(item);
  ink.editing=item.editing;ink.clipLeft=x;ink.clipRight=textRight;
  // Keep the full caption; only its selected text window moves. Icons and
  // entry cues remain stationary, and short captions never change size.
  const int offset=i==selected?textOffset(item,focusedMs):0;
  proportional::atBaseline(ink,label,x-offset,proportional::rowBaseline(y,layout.height,fontSize),fontSize,0,0xffff);
  ink.editing=false;ink.clipLeft=0;ink.clipRight=280;
  if(item.icon){
   const int size=item.iconSize,xx0=44-size/2,yy0=y+(layout.height-size)/2;
   for(int iy=0;iy<size;++iy)for(int ix=0;ix<size;++ix){
    int yy=yy0+iy,xx=xx0+ix;if(yy<layout.top||yy>=layout.bottom)continue;
    int bit=iy*size+ix;unsigned a=((item.icon[bit/2]>>(bit%2?0:4))&15)*17;
    if(!a)continue;
    const unsigned cover=ink.coverage(xx,yy);
    const auto bg=rounded_fill::blend(0,0xffff,cover);
    const auto fg=item.iconColor?item.iconColor:rounded_fill::blend(0xffff,0x9492,cover);
    c.fillRect(xx,yy,1,1,rounded_fill::blend(fg,bg,a));
   }
  }
  if(!item.choice&&item.opensScreen)for(int d=0;d<6;++d){ink.fillRect(249+d,y+layout.height/2-6+d,2,2,0);ink.fillRect(249+d,y+layout.height/2+4-d,2,2,0);}
  else if(item.checked)for(int dy=-3;dy<=3;++dy)for(int dx=-3;dx<=3;++dx)if(dx*dx+dy*dy<=9)ink.fillRect(252+dx,y+layout.height/2+dy,1,1,0);
 }
}
struct State {
 Motion motion;int selected=-1,first=-1;bool moving=false;unsigned lastFrame=0;
 unsigned focusAt=0,lastSeen=0;int lastTextOffset=-1;char focusedText[160]={};
 template<class C,class List,class Items> bool draw(C& c,List& list,Items items,unsigned now,bool dirty){
  list.visibleRows=4;list.clampScroll();
  const auto focus=items(list.selected);
  const char* label=focus.label?focus.label:"";
  if(selected!=list.selected || strcmp(focusedText,label) || now-lastSeen>200){
   strncpy(focusedText,label,sizeof(focusedText)-1);focusedText[sizeof(focusedText)-1]=0;focusAt=now;
  }
  lastSeen=now;
  const int nextTextOffset=textOffset(focus,now-focusAt);
  if(nextTextOffset!=lastTextOffset)dirty=true;
  if(selected<0){motion.reset(list.selected,list.scrollFirst,now);dirty=true;}
  else if(selected!=list.selected||first!=list.scrollFirst){motion.move(list.selected,list.scrollFirst,now);moving=true;dirty=true;}
  selected=list.selected;first=list.scrollFirst;
  if(moving && now-lastFrame>=16)dirty=true;
  if(!dirty)return false;
  paint(c,motion.sample(now),list.totalItems,list.selected,items,now-focusAt);
  lastTextOffset=nextTextOffset;
  lastFrame=now;if(now-motion.at>=motion.durationMs)moving=false;
  return true;
 }
};
}}
