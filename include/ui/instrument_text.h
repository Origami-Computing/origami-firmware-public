#pragma once
#ifdef ORIGAMI_INSTRUMENT_FONT
#include <Fonts/FlashFont.h>
namespace ui { namespace instrument_text {
inline uint16_t blend(uint16_t fg,uint16_t bg,unsigned a) {
    const unsigned inv=255-a;
    return (((((fg>>11)*a+(bg>>11)*inv+127)/255)&31)<<11) |
           ((((((fg>>5)&63)*a+((bg>>5)&63)*inv+127)/255)&63)<<5) |
           ((((fg&31)*a+(bg&31)*inv+127)/255)&31);
}
// TFT surfaces preserve their actual backgrounds. Geometry-only animation wrappers
// paint on white and apply their own opacity after this coverage compositing.
template<class C> auto background(C& c,int x,int y,int)->decltype(c.readPixel(x,y)) { return c.readPixel(x,y); }
template<class C> uint16_t background(C&,int,int,long) { return 0xFFFF; }
inline const instrument::Font& cell(int scale) {
    // Existing fixed-grid callers use only 1, 2 and 5; add a matching raster
    // before introducing another scale so their width contract stays exact.
    return scale==2?instrument::cell2:scale==5?instrument::cell5:instrument::fonts[0];
}
inline int width(const char* text,const instrument::Font& f) {
    int w=0;for(;*text;++text) w+=f.glyphs[(static_cast<unsigned char>(*text)>=32 && static_cast<unsigned char>(*text)<=126?static_cast<unsigned char>(*text):63)-32].advance;
    return w;
}
template<class C> void text(C& c,const char* value,int x,int y,const instrument::Font& f,uint16_t fg) {
    for(const unsigned char* p=(const unsigned char*)value;*p;++p) {
        const auto& g=f.glyphs[(*p>=32 && *p<=126?*p:63)-32];
        for(int row=0;row<g.height;++row)for(int col=0;col<g.width;++col) {
            const uint8_t a=f.pixels[g.offset+row*g.width+col]; if(!a)continue;
            int px=x+g.x+col,py=y+f.ascent+g.y+row;
            c.fillRect(px,py,1,1,a==255?fg:blend(fg,background(c,px,py,0),a));
        }
        x+=g.advance;
    }
}
} }
#endif
