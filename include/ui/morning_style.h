#pragma once

#include "ui/scene.h"
#include "ui/kami_message.h"
#include "ui/safe_area_layout.h"
#include "ui/chibi_eyes.h"
#include "ui/home_eyes.h"
#include <stdio.h>
#include <string.h>

namespace ui { namespace morning_style {

constexpr uint16_t kBg = 0xFFFF;
constexpr uint16_t kInk = 0x0000;
constexpr uint16_t kActionBg = 0xF7BE; // Very quiet cool grey, shared across the flow.
constexpr int kFooterH = 34;

enum class Icon { Next, Home, Snooze, Wake, Skip, Mic, Cancel, Save, Retry };

enum class StatusMark { Success, Error, Loading };

inline void statusMark(scene::Scene& s, int width, StatusMark kind,
                       uint16_t color) {
  const int x=width/2;
  s.addCircle("status.mark",x,78,25,color,1,color,true,1);
  if(kind==StatusMark::Success) {
    s.addLine("status.check.a",x-10,78,x-2,87,kInk,2);
    s.addLine("status.check.b",x-2,87,x+13,68,kInk,2);
  } else {
    s.addLabel("status.symbol",{(int16_t)(x-24),58,48,40},
        kind==StatusMark::Error?"!":"...",kInk,color,
        kind==StatusMark::Error?3:2,scene::TextAlign::MiddleCenter,2);
  }
}

// Small, consistent 16px line icons. No font-dependent Unicode glyphs.
inline void icon(scene::Scene& s, const char* prefix, int x, int y, Icon kind) {
  int n = 0;
  auto line = [&](int a, int b, int c, int d) {
    char id[32]; snprintf(id, sizeof(id), "%s.i%d", prefix, n++);
    s.addLine(id, x+a, y+b, x+c, y+d, kInk, 4);
  };
  switch (kind) {
    case Icon::Next: case Icon::Skip: case Icon::Retry:
      line(1,8,14,8); line(9,3,14,8); line(9,13,14,8);
      if (kind == Icon::Skip) line(16,3,16,13);
      if (kind == Icon::Retry) { line(1,8,1,2); line(1,2,10,2); }
      break;
    case Icon::Cancel: line(2,2,14,14); line(2,14,14,2); break;
    case Icon::Home:
      line(0,7,8,0); line(8,0,16,7); line(3,6,3,15); line(3,15,13,15);
      line(13,15,13,6); line(7,15,7,10); line(7,10,10,10); line(10,10,10,15); break;
    case Icon::Save:
      line(1,1,13,1); line(13,1,15,3); line(15,3,15,15); line(15,15,1,15);
      line(1,15,1,1); line(5,1,5,6); line(5,6,11,6); line(11,6,11,1);
      line(4,15,4,10); line(4,10,12,10); line(12,10,12,15); break;
    case Icon::Mic:
      line(6,1,10,1); line(6,1,6,9); line(10,1,10,9); line(6,9,10,9);
      line(3,7,3,10); line(3,10,8,13); line(8,13,13,10); line(13,10,13,7);
      line(8,13,8,16); line(4,16,12,16); break;
    case Icon::Snooze:
      line(3,3,13,3); line(13,3,3,13); line(3,13,13,13); break;
    case Icon::Wake:
      { char id[32]; snprintf(id,sizeof(id),"%s.sun",prefix);
        s.addCircle(id,x+8,y+8,4,kInk,1,0,false,4); }
      line(8,0,8,2); line(8,14,8,16); line(0,8,2,8); line(14,8,16,8);
      line(2,2,3,3); line(13,13,14,14); line(2,14,3,13); line(13,3,14,2); break;
  }
}

// Retain caller button mappings; all action panels share the dialogue frame.
inline void footer(scene::Scene& s, int w, int h, const char* right, Icon,
                   const char* left=nullptr, Icon=Icon::Cancel) {
  kami_message::footer(s,w,h,right,left);
}

// Font 1 is the firmware's fixed 6x8 bitmap face. Break at word boundaries,
// hard-wrap long tokens, and report overflow instead of silently dropping text.
struct TextLayout { char lines[32][48]{}; int count=0; int size=1; bool fits=false; };
inline TextLayout fitText(const char* text, int width, int height, int maxSize=2) {
  TextLayout result;
  for(int size=maxSize;size>=1;--size) {
    result=TextLayout{}; result.size=size;
    int columns=width/(6*size); if(columns>47) columns=47;
    if(columns<1) continue;
    const char* p=text ? text : "";
    while(*p && result.count<32) {
      while(*p==' ' || *p=='\n') ++p;
      if(!*p) break;
      int n=0,space=-1;
      while(p[n] && p[n]!='\n' && n<columns) { if(p[n]==' ') space=n; ++n; }
      if(p[n] && p[n]!='\n' && p[n]!=' ' && n==columns && space>0) n=space;
      memcpy(result.lines[result.count],p,n); result.lines[result.count++][n]=0; p+=n;
    }
    result.fits=!*p && result.count*(8*size+2)<=height;
    if(result.fits) break;
  }
  return result;
}

inline void text(scene::Scene& s,const char* id,const char* value,Rect box,int maxSize=2) {
  const auto fitted=fitText(value,box.w,box.h,maxSize);
  const int lineH=fitted.size*8+2;
  const int y=box.y+(box.h-fitted.count*lineH)/2;
  for(int i=0;i<fitted.count;++i) {
    char node[32]; snprintf(node,sizeof(node),"%s.%d",id,i);
    s.addLabel(node,{box.x,(int16_t)(y+i*lineH),box.w,(int16_t)lineH},fitted.lines[i],
        kInk,kBg,fitted.size,scene::TextAlign::MiddleCenter,2);
  }
}

// The chibi frame's visible pair spans 96 of its 120 pixels. Match that ink
// width to the home BMP's 230px pair at the canonical home-shell scale.
inline void eyes(scene::Scene& s,int width,int centerY,chibi_eyes::Expression expression,
                 int heightPercent=100,int rotation=0,int scalePercent=100) {
  const int inkW=home_eyes::kCanonicalInkWidth*scalePercent/100;
  const int w=inkW*120/96, h=w*102/120*heightPercent/100;
  s.addBitmap("morning.eyes",{(int16_t)((width-w)/2),(int16_t)(centerY-h/2),(int16_t)w,(int16_t)h},
      chibi_eyes::bitmap(expression,rotation),120,102,kInk,kBg,1,4);
}

}} // namespace ui::morning_style
