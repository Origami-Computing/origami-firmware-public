#pragma once
#include <TB_TFT_eSPI.h>
#include "assets/doodle_notebook.h"

namespace doodle_draw {
constexpr uint16_t rgb(unsigned r, unsigned g, unsigned b) {
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
constexpr uint16_t paper = rgb(245,239,220);
constexpr uint16_t ink = rgb(53,53,42);
constexpr uint16_t panel = rgb(248,242,223);
constexpr uint16_t green = rgb(166,204,70);
constexpr uint16_t button = rgb(224,229,165);

inline uint16_t blend(uint16_t fg, uint16_t bg, unsigned a) {
  const unsigned inv=255-a;
  return (((((fg>>11)*a+(bg>>11)*inv+127)/255)&31)<<11) |
         ((((((fg>>5)&63)*a+((bg>>5)&63)*inv+127)/255)&63)<<5) |
         ((((fg&31)*a+(bg&31)*inv+127)/255)&31);
}
inline void pixel(TFT_eSprite& c,int x,int y,uint16_t color,uint8_t alpha) {
  if (!alpha || x<0 || x>=c.width() || y<38 || y>=c.height()) return;
  c.drawPixel(x,y,alpha==255?color:blend(color,c.readPixel(x,y),alpha));
}
inline int textWidth(const char* text,const doodle_art::Font& font) {
  int width=0;
  for (;*text;++text) {unsigned ch=(unsigned char)*text;if(ch>=32&&ch<127)width+=font.glyphs[ch-32].advance;}
  return width;
}
// The preview's text coordinates are alphabetic baselines; no opaque text box.
inline void text(TFT_eSprite& c,const char* value,int x,int baseline,const doodle_art::Font& font,uint16_t color=ink,int align=0) {
  const int width=textWidth(value,font);
  if(align==1)x-=width/2;else if(align==2)x-=width;
  for(;*value;++value){
    unsigned ch=(unsigned char)*value;if(ch<32||ch>=127)continue;
    const auto& g=font.glyphs[ch-32];
    for(int y=0;y<g.h;++y)for(int dx=0;dx<g.w;++dx)
      pixel(c,x+g.x+dx,baseline+g.y+y,color,font.alpha[g.offset+y*g.w+dx]);
    x+=g.advance;
  }
}
inline void sprite(TFT_eSprite& c,const doodle_art::Sprite& image,int x,int y,bool flip=false) {
  for(int dy=0;dy<image.h;++dy)for(int dx=0;dx<image.w;++dx){
    int i=dy*image.w+dx;pixel(c,x+(flip?image.w-1-dx:dx),y+dy,image.pixels[i],image.alpha[i]);
  }
}
inline void platform(TFT_eSprite& c,int x,int y,int w=46) {
  c.fillRoundRect(x,y,w,8,3,green);
  c.drawRoundRect(x,y,w,8,3,ink);
  c.drawLine(x+4,y+2,x+w-6,y+1,rgb(209,226,123));
  for(int dx=7;dx<w-5;dx+=5)c.drawLine(x+dx,y+3,x+dx-2,y+5,rgb(99,140,52));
  c.drawLine(x+6,y+6,x+w-7,y+6,rgb(101,134,52));
}
inline void oval(TFT_eSprite& c,int x,int y,int rx,int ry) {
  c.fillEllipse(x,y,rx,ry,button);c.drawEllipse(x,y,rx,ry,ink);
}
inline void background(TFT_eSprite& c) {
  c.fillRect(0,38,c.width(),c.height()-38,paper);
  const auto grid=rgb(232,230,210);
  for(int x=1;x<c.width();x+=7)c.drawFastVLine(x,38,c.height()-38,grid);
  for(int y=40;y<c.height();y+=7)c.drawFastHLine(0,y,c.width(),grid);
}
inline void ready(TFT_eSprite& c) {
  text(c,"doodle",140,101,doodle_art::font30,rgb(112,138,50),1);
  text(c,"jump",139,129,doodle_art::font30,ink,1);
  sprite(c,doodle_art::readyPlayer,38,145);
  platform(c,33,200,55);
  oval(c,184,161,55,17);
  text(c,"press dial",184,158,doodle_art::font13,ink,1);
  text(c,"to start",184,172,doodle_art::font13,ink,1);
}
inline void gameOver(TFT_eSprite& c,uint32_t score,uint32_t best) {
  c.fillTriangle(39,83,239,81,241,211,panel);
  c.fillTriangle(39,83,241,211,37,214,panel);
  c.drawLine(39,83,239,81,ink);c.drawLine(239,81,241,211,ink);
  c.drawLine(241,211,37,214,ink);c.drawLine(37,214,39,83,ink);
  text(c,"game over",140,114,doodle_art::font25,ink,1);
  char line[40];
  snprintf(line,sizeof(line),"your score: %lu",(unsigned long)score);
  text(c,line,140,143,doodle_art::font16,ink,1);
  snprintf(line,sizeof(line),"your best: %lu",(unsigned long)best);
  text(c,line,140,165,doodle_art::font14,ink,1);
  oval(c,140,192,90,14);
  text(c,"press dial to play again",140,197,doodle_art::font13,ink,1);
}
}
