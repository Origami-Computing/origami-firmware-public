#pragma once
#include <stdint.h>

namespace hal { namespace display {
// UI-task invalidation only. Retain horizontal extent in each existing 8-row
// band, so a right-hand indicator cannot dirty unchanged text in the center.
struct DirtyBands {
  static constexpr int Width=280, Height=240, BandHeight=8, Count=30;
  uint32_t bits=0;
  uint16_t left[Count]{}, right[Count]{};
  void clear() { bits=0; }
  void add(int x,int y,int w,int h) {
    if(w<=0 || h<=0) return;
    const int64_t farX=int64_t(x)+w, farY=int64_t(y)+h;
    if(x>=Width || y>=Height || farX<=0 || farY<=0) return;
    const int x0=x<0?0:x, y0=y<0?0:y;
    const int x1=farX>Width?Width:int(farX), y1=farY>Height?Height:int(farY);
    for(int b=y0/BandHeight;b<=(y1-1)/BandHeight;++b) {
      const uint32_t mask=uint32_t(1)<<b;
      if(!(bits&mask)) { left[b]=x0; right[b]=x1; }
      else { if(x0<left[b])left[b]=x0; if(x1>right[b])right[b]=x1; }
      bits|=mask;
    }
  }
  struct Window { int x,y,w,h; };
  // Cursor is a band index. Only merge adjacent bands with equal extents;
  // clean gaps and neighboring content must not expand a status-field update.
  bool next(int& cursor,Window& out) const {
    while(cursor<Count && !(bits&(uint32_t(1)<<cursor))) ++cursor;
    if(cursor==Count)return false;
    const int first=cursor++;
    while(cursor<Count && (bits&(uint32_t(1)<<cursor)) &&
          left[cursor]==left[first] && right[cursor]==right[first]) ++cursor;
    out={left[first],first*BandHeight,right[first]-left[first],(cursor-first)*BandHeight};
    return true;
  }
};
} }
