#pragma once
#include "assets/ui_text.h"
#include <algorithm>
#include <cstring>
#include <stdint.h>

// Measured, bounded text rendering shared by live UI and host evidence fixtures.
// Sizes are actual pixels; existing fixed-cell text contracts remain unchanged.
namespace ui { namespace proportional {
inline const assets::ui_text::Font& font(int px=18) { switch(px){case 12:return assets::ui_text::font12;case 14:return assets::ui_text::font14;case 16:return assets::ui_text::font16;case 20:return assets::ui_text::font20;case 28:return assets::ui_text::font28;case 44:return assets::ui_text::font44;case 60:return assets::ui_text::font60;default:return assets::ui_text::font18;} }
inline const assets::ui_text::Glyph& glyph(unsigned char c,int px=18){return font(px).glyphs[(c>=32&&c<127?c:'?')-32];}
inline int width(const char* s,int px=18){unsigned n=0;for(;*s;++s)n+=glyph(*s,px).advance;return (n+32)/64;}
inline int top(const char* s,int px=18){int y=0;for(;*s;++s)if(*s!=' ')y=std::min(y,int(glyph(*s,px).y));return y;}
inline int height(const char* s,int px=18){int b=0;for(const char* p=s;*p;++p)if(*p!=' ')b=std::max(b,int(glyph(*p,px).y+glyph(*p,px).h));return b-top(s,px);}
inline uint16_t blend(uint16_t fg,uint16_t bg,unsigned a){return (((((fg>>11)*a+(bg>>11)*(255-a)+127)/255)&31)<<11)|((((((fg>>5)&63)*a+((bg>>5)&63)*(255-a)+127)/255)&63)<<5)|(((fg&31)*a+(bg&31)*(255-a)+127)/255);}
template<class C> void text(C& c,const char* s,int x,int y,int px=18,uint16_t ink=0,uint16_t paper=0xffff){
 const auto& f=font(px);const int baseline=y-top(s,px);unsigned advance=0;
 for(;*s;++s){const auto& g=glyph(*s,px);int xx=x+(advance+32)/64+g.x;
  for(int row=0;row<g.h;++row)for(int col=0;col<g.w;++col){unsigned a=f.pixels[g.offset+row*g.w+col];if(a)c.fillRect(xx+col,baseline+g.y+row,1,1,blend(ink,paper,a));}
  advance+=g.advance;
 }
}
template<class C> void centered(C& c,const char* s,int y,int px=18,uint16_t ink=0,uint16_t paper=0xffff,int cx=140){text(c,s,cx-width(s,px)/2,y,px,ink,paper);}
struct Line {unsigned bytes,next;};
inline Line line(const char* s,int maxWidth,int px=18){
 unsigned n=0,space=0,advance=0;
 while(s[n]&&s[n]!='\n'&&n<127){unsigned a=glyph(s[n],px).advance;if(advance+a>unsigned(maxWidth*64)&&n)break;if(s[n]==' ')space=n;advance+=a;++n;}
 if(s[n]&&s[n]!='\n'&&s[n]!=' '&&space)n=space;
 return {n,n+unsigned(s[n]==' '||s[n]=='\n')};
}
template<class C> unsigned paragraph(C& c,const char* s,int x,int y,int w,int rows,int px=18,int pitch=20,uint16_t ink=0,uint16_t paper=0xffff){
 unsigned count=0;while(*s&&count<unsigned(rows)){auto l=line(s,w,px);if(!l.next)break;char value[128];memcpy(value,s,l.bytes);value[l.bytes]=0;text(c,value,x,y+count*pitch,px,ink,paper);s+=l.next;++count;}return count;
}
}}
