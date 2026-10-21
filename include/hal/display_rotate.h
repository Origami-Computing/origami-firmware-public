#pragma once
#include <stdint.h>

namespace hal { namespace display {
// Both buffers live in PSRAM. Tile the transpose so dirty-window strides
// (especially 128 pixels) cannot evict every destination cache line per row.
inline void rotateRect(const uint16_t* src,uint16_t* dst,int width,
                       int firstX,int lastX,int firstY,int lastY) {
  const int stride=lastY-firstY;
  for(int y0=firstY;y0<lastY;y0+=16) {
    const int y1=y0+16<lastY?y0+16:lastY;
    for(int x0=firstX;x0<lastX;x0+=16) {
      const int x1=x0+16<lastX?x0+16:lastX;
      for(int x=x0;x<x1;++x) {
        uint16_t* out=dst+(lastX-1-x)*stride+y0-firstY;
        for(int y=y0;y<y1;++y) {
          const uint16_t px=src[y*width+x];
          *out++=(uint16_t)((px>>8)|(px<<8));
        }
      }
    }
  }
}
inline void rotateWindow(const uint16_t* src,uint16_t* dst,int width,int firstY,int lastY) {
  rotateRect(src,dst,width,0,width,firstY,lastY);
}
}}
