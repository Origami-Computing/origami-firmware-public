#pragma once
#include <stdint.h>
namespace ui { namespace raster {
// RGB565 colors with integer coverage 0..15. Canvas must supply fillRect;
// maskOver additionally reads existing pixels through at(x,y); its rectangle
// must fit the readable surface (fillRect clipping alone is insufficient).
// Packed masks use the high nibble first; width/height must be positive.
inline uint16_t blend(uint16_t fg,uint16_t bg,int a){
 int r=(((fg>>11)&31)*a+((bg>>11)&31)*(15-a)+7)/15;
 int g=(((fg>>5)&63)*a+((bg>>5)&63)*(15-a)+7)/15;
 int b=((fg&31)*a+(bg&31)*(15-a)+7)/15;
 return (r<<11)|(g<<5)|b;
}
template<class C>void mask(C& c,const uint8_t* data,int x,int y,int w,int h,uint16_t fg,uint16_t bg,int drawHeight=0){
 if(!drawHeight)drawHeight=h;
 for(int row=0;row<drawHeight;row++)for(int col=0;col<w;col++){
  int i=(row*h/drawHeight)*w+col;int a=(data[i/2]>>(i%2?0:4))&15;
  c.fillRect(x+col,y+row,1,1,blend(fg,bg,a));
 }
}
template<class C>void maskOver(C& c,const uint8_t* data,int x,int y,int w,int h,uint16_t ink){
 for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){
  int i=yy*w+xx,a=(data[i/2]>>(i%2?0:4))&15;
  if(a)c.fillRect(x+xx,y+yy,1,1,blend(ink,c.at(x+xx,y+yy),a));
 }
}
} }
