#pragma once
#include "ui/scene.h"
#include "ui/reflection_layout.h"
#include "ui/default_font_metrics.h"
#include "assets/kami_dialogue.h"
#include "config/ui_theme.h"
#include <stdio.h>
#include <string.h>

namespace ui { namespace kami_message {
inline Rect rect(safe_area::Box b) {
  return {(int16_t)b.x,(int16_t)b.y,(int16_t)b.w,(int16_t)b.h};
}
inline void frame(scene::Scene& s,const char* id,safe_area::Box box,bool speech=false,uint8_t z=1) {
  auto* n=s.addRect(id,rect(box),kUiBg,0,z);
  if(n) {
    n->frameStyle=speech?scene::FrameStyle::Speech:scene::FrameStyle::Dialogue;
    n->strokeColor=kUiFg;n->strokeWidth=2;
  }
}
inline void action(scene::Scene& s,const char* id,safe_area::Box box,const char* label) {
  frame(s,id,box,false,3);
  const uint8_t size=default_font::width(label,2)<=box.w-20?2:1;
  char textId[32];snprintf(textId,sizeof(textId),"%s.label",id);
  s.addLabel(textId,{(int16_t)(box.x+8),(int16_t)(box.y+6),(int16_t)(box.w-16),(int16_t)(box.h-12)},
      label,kUiFg,kUiBg,size,scene::TextAlign::MiddleCenter,4);
}
inline void footer(scene::Scene& s,int width,int height,const char* right,const char* left=nullptr) {
  const auto p=reflection::layout(width,height);
  if(left) {
    // Give long actions (Download, Dismiss) room while keeping one shared rail.
    const int leftNeed=default_font::width(left,2)+24;
    const int rightNeed=default_font::width(right,2)+24;
    const int available=p.dialogue.w-8;
    const int leftWidth=available*leftNeed/(leftNeed+rightNeed);
    action(s,"actions.a",{p.skip.x,p.skip.y,leftWidth,p.skip.h},left);
    action(s,"actions.b",{p.skip.x+leftWidth+8,p.skip.y,available-leftWidth,p.skip.h},right);
  } else action(s,"actions.b",p.talk,right);
}
inline unsigned lines(const char* text,int width,uint8_t size) {
  unsigned count=0;
  while(*text) {
    const auto line=default_font::line(text,width,size);
    if(!line.next)return 1000;
    text+=line.next;++count;
  }
  return count;
}
inline void footer3(scene::Scene& s,int width,int height,const char* a,const char* b,const char* c) {
  const auto p=reflection::layout(width,height);
  const int needA=default_font::width(a,1)+20,needB=default_font::width(b,1)+20;
  const int total=needA+needB+default_font::width(c,1)+20;
  const int available=p.dialogue.w-16;
  const int wa=available*needA/total,wb=available*needB/total;
  action(s,"actions.a",{p.skip.x,p.skip.y,wa,34},a);
  action(s,"actions.b",{p.skip.x+wa+8,p.skip.y,wb,34},b);
  action(s,"actions.c",{p.skip.x+wa+wb+16,p.skip.y,available-wa-wb,34},c);
}
// Measured wrapping uses the same font as scene labels, including long tokens.
// Returns false if the supplied message exceeds even the small-font capacity.
inline bool message(scene::Scene& s,int width,int height,const char* id,const char* text) {
  const auto p=reflection::layout(width,height);
  s.addBitmap("dialogue.avatar",rect(p.avatar),assets::kami_dialogue::kBody,24,26,0xF945,kUiBg,1,4);
  s.addBitmap("dialogue.eyes",rect(p.eyes),assets::kami_dialogue::kEyes,16,10,kUiFg,0xF945,2,4);
  frame(s,"dialogue.bubble",p.dialogue,true);
  const char* remaining=text?text:"";
  uint8_t size=2;
  const unsigned count=lines(remaining,p.text.w,size);
  if(count && count*(default_font::height(size)+1)-1>unsigned(p.text.h+2))size=1;
  const int pitch=default_font::height(size)+1;
  int y=p.text.y;
  unsigned index=0;
  while(*remaining && y+default_font::height(size)<=p.text.y+p.text.h+2) {
    const auto line=default_font::line(remaining,p.text.w,size);
    if(!line.next)break;
    char value[128],node[32];
    memcpy(value,remaining,line.bytes);value[line.bytes]=0;
    snprintf(node,sizeof(node),"%s.%u",id,index++);
    s.addLabel(node,{(int16_t)p.text.x,(int16_t)y,(int16_t)p.text.w,(int16_t)default_font::height(size)},
        value,kUiFg,kUiBg,size,scene::TextAlign::TopLeft,2);
    remaining+=line.next;y+=pitch;
  }
  return !*remaining;
}
}} // namespace ui::kami_message
