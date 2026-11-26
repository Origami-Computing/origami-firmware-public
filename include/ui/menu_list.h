#pragma once
#include "ui/menu_motion.h"
#include "ui/focused_label.h"
#include "ui/list_row.h"
#include "ui/proportional_text.h"
#include "ui/rounded_fill.h"
#include "config/ui_theme.h"
#include <cmath>

// Shared four-row menu painter. Uses the existing surface, native icon masks,
// and ListView's input/window state; no extra framebuffer or navigation stack.
namespace ui { namespace menu_list {
struct Layout {int top=16,pitch=56,height=50,bottom=234;};
constexpr int fontSize=20, textLeft=68, textRight=246;
// Canvas rows [y0,y1) a draw changed. Empty when nothing needs a transfer.
struct Band {
 int y0=0,y1=0;
 constexpr Band()=default;
 constexpr Band(int top,int bottom):y0(top),y1(bottom){}
 explicit operator bool()const{return y1>y0;}
 // Adds a fractional span plus a pixel of anti-aliased edge either side.
 void add(float a,float b){
  const int lo=std::max(0,int(std::floor(a))-1),hi=std::min(240,int(std::ceil(b))+1);
  if(hi<=lo)return;
  if(!*this){y0=lo;y1=hi;}else{y0=std::min(y0,lo);y1=std::max(y1,hi);}
 }
};
constexpr Band kFullFrame{0,240};
inline int textX(const ListItem& item){return item.icon?textLeft:32;}
inline int textOffset(const ListItem& item,unsigned elapsed){return focused_label::offset(elapsed,proportional::width(item.label?item.label:"",fontSize)-(textRight-textX(item)));}
template<class C> struct Ink {
 C& c; float y0,y1; bool highlight; bool editing; int clipLeft,clipRight; const Layout& layout; int clipTop,clipBottom;
 unsigned coverage(int x,int y)const{return highlight?rounded_fill::coverage({12,y0,256,y1-y0,float(layout.height)/2},x,y):0;}
 void fillRect(int x,int y,int w,int h,uint16_t color){
  const int x0=std::max(clipLeft,x),x1=std::min(clipRight,x+w);
  for(int yy=std::max(clipTop,y);yy<std::min(clipBottom,y+h);++yy){
   // Equal neighbouring pixels go out as one span: same pixels, fewer calls.
   int run=x0;uint16_t runColor=0;
   for(int xx=x0;xx<x1;++xx){
    const uint16_t pixel=editing
      ?rounded_fill::blend(kUiEdit,rounded_fill::blend(0,0xffff,coverage(xx,yy)),255-unsigned(color>>11)*255/31)
      :rounded_fill::blend(uint16_t(~color),color,coverage(xx,yy));
    if(xx>run&&pixel!=runColor){c.fillRect(run,yy,xx-run,1,runColor);run=xx;}
    runColor=pixel;
   }
   if(x1>run)c.fillRect(run,yy,x1-run,1,runColor);
  }
 }
};
template<class C,class Items> void paint(C& c,Pose p,int count,int selected,Items itemAt,unsigned focusedMs=0,Layout layout=Layout{},Band clip=kFullFrame){
 // Only rows inside clip are cleared and repainted; the result inside the
 // band is identical to a full-frame paint.
 if(!clip)return;
 c.fillRect(0,clip.y0,280,clip.y1-clip.y0,0xffff);
 const int clipTop=std::max(layout.top,clip.y0),clipBottom=std::min(layout.bottom,clip.y1);
 if(clipBottom<=clipTop)return;
 const auto selection=count?itemAt(selected):ListItem{};
 const bool highlight=count && (!selection.editing||selection.blinkOn);
 const float y0=p.top-p.scroll,y1=p.bottom-p.scroll;
 if(highlight)rounded_fill::draw(c,{12,y0,256,y1-y0,float(layout.height)/2},0,0xffff,{0,clipTop,280,clipBottom-clipTop});
 Ink<C> ink{c,y0,y1,highlight,false,0,280,layout,clipTop,clipBottom};
 for(int i=0;i<count;++i){
  int y=int(std::round(layout.top+i*layout.pitch-p.scroll));if(y+layout.height<=clipTop||y>=clipBottom)continue;
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
   for(int iy=0;iy<size;++iy){
    const int yy=yy0+iy;if(yy<clipTop||yy>=clipBottom)continue;
    int run=-1;uint16_t runColor=0;
    for(int ix=0;ix<=size;++ix){
     const int xx=xx0+ix,bit=iy*size+ix;
     const unsigned a=ix<size?((item.icon[bit/2]>>(bit%2?0:4))&15)*17:0;
     uint16_t pixel=0;
     if(a){
      const unsigned cover=ink.coverage(xx,yy);
      const auto bg=rounded_fill::blend(0,0xffff,cover);
      const auto fg=item.iconColor?item.iconColor:rounded_fill::blend(0xffff,0x9492,cover);
      pixel=rounded_fill::blend(fg,bg,a);
     }
     if(run>=0&&(!a||pixel!=runColor)){c.fillRect(xx0+run,yy,ix-run,1,runColor);run=-1;}
     if(a&&run<0){run=ix;runColor=pixel;}
    }
   }
  }
  if(!item.choice&&item.opensScreen)for(int d=0;d<6;++d){ink.fillRect(249+d,y+layout.height/2-6+d,2,2,0);ink.fillRect(249+d,y+layout.height/2+4-d,2,2,0);}
  else if(item.checked)for(int dy=-3;dy<=3;++dy)for(int dx=-3;dx<=3;++dx)if(dx*dx+dy*dy<=9)ink.fillRect(252+dx,y+layout.height/2+dy,1,1,0);
 }
}
// Draws when something changed and returns the rows to transfer: the band the
// highlight swept since the last paint, the focused row for marquee/icon
// frames (focusDirty), or the whole frame for scrolling, entry and dirty.
struct State {
 Motion motion;Pose painted{};int selected=-1,first=-1;bool moving=false;unsigned lastFrame=0;
 unsigned focusAt=0,lastSeen=0;int lastTextOffset=-1;uint32_t focusedHash=0;
 // FNV-1a of the focused label: restarts its marquee when the text changes
 // without retaining a per-menu copy in internal RAM.
 static uint32_t labelHash(const char* s){uint32_t h=2166136261u;while(*s)h=(h^(uint8_t)*s++)*16777619u;return h;}
 template<class C,class List,class Items> Band draw(C& c,List& list,Items items,unsigned now,bool dirty,bool focusDirty=false){
  list.visibleRows=4;list.clampScroll();
  const auto focus=items(list.selected);
  const char* label=focus.label?focus.label:"";
  const uint32_t hash=labelHash(label);
  if(selected!=list.selected || hash!=focusedHash || now-lastSeen>200){
   focusedHash=hash;focusAt=now;
  }
  lastSeen=now;
  const int nextTextOffset=textOffset(focus,now-focusAt);
  const bool textMoved=nextTextOffset!=lastTextOffset;
  bool step=false;const int previous=selected;
  if(selected<0){motion.reset(list.selected,list.scrollFirst,now);dirty=true;}
  else if(selected!=list.selected||first!=list.scrollFirst){motion.move(list.selected,list.scrollFirst,now);moving=true;step=true;}
  selected=list.selected;first=list.scrollFirst;
  if(moving && now-lastFrame>=16)step=true;
  if(!dirty&&!step&&!textMoved&&!focusDirty)return {};
  const Pose pose=motion.sample(now);
  Band band;
  if(dirty||pose.scroll!=painted.scroll)band=kFullFrame;
  else{
   if(step){band.add(painted.top-painted.scroll,painted.bottom-painted.scroll);band.add(pose.top-pose.scroll,pose.bottom-pose.scroll);}
   // A new focus changes both rows' icon and caption at once, ahead of the highlight.
   auto addRow=[&](int i){const float y=float(top+i*pitch)-pose.scroll;band.add(y,y+rowHeight);};
   if(textMoved||focusDirty)addRow(list.selected);
   if(previous!=list.selected){addRow(previous);addRow(list.selected);}
  }
  paint(c,pose,list.totalItems,list.selected,items,now-focusAt,Layout{},band);
  painted=pose;lastTextOffset=nextTextOffset;
  lastFrame=now;if(now-motion.at>=motion.durationMs)moving=false;
  return band;
 }
};
}}
