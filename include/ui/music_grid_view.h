#pragma once
// Shared music typography, rendered identically on the host and ESP32.
#include "ui/music_player_view.h"
#ifndef ARDUINO
#include "../../lib/TB_TFT_eSPI/Fonts/GFXFF/FreeSansBold12pt7b.h"
#include "../../lib/TB_TFT_eSPI/Fonts/GFXFF/FreeSans9pt7b.h"
#endif
#include <string>
namespace music_grid {
inline int width(const std::string& s,const GFXfont& font) {
#ifdef ORIGAMI_INSTRUMENT_FONT
    return ui::instrument_text::width(s.c_str(),instrument::fonts[&font==&FreeSans9pt7b?1:2]);
#else
    int w=0;for(unsigned char ch:s)w+=font.glyph[(ch>=32&&ch<=126?ch:'?')-32].xAdvance;
    return w;
#endif
}
template<class C> void label(C& c,const std::string& s,int x,int baseline,const GFXfont& font,uint16_t color) {
#ifdef ORIGAMI_INSTRUMENT_FONT
    const auto& face=instrument::fonts[&font==&FreeSans9pt7b?1:2];
    ui::instrument_text::text(c,s.c_str(),x,baseline-face.ascent,face,color);
#else
    for(unsigned char ch:s) {
        const auto& g=font.glyph[(ch>=32&&ch<=126?ch:'?')-32];
        for(int y=0;y<g.height;y++)for(int col=0;col<g.width;col++) {
            int bit=y*g.width+col;
            if(font.bitmap[g.bitmapOffset+bit/8]&(128>>(bit%8)))
                c.fillRect(x+g.xOffset+col,baseline+g.yOffset+y,1,1,color);
        }
        x+=g.xAdvance;
    }
#endif
}

inline std::string ellipsis(std::string s,const GFXfont& f,int limit) {
    if(width(s,f)<=limit)return s;
    while(!s.empty()&&width(s+"...",f)>limit)s.pop_back();
    return s+"...";
}
template<class C> void render(C& c,const music_view::State& s) {
    c.fillRect(0,0,280,240,kUiBg);
    // Header center is the screen center; side icons map to physical controls.
    std::string clock=s.clock&&s.clock[0]?s.clock:"--:--";
    label(c,clock,140-width(clock,FreeSans9pt7b)/2,29,FreeSans9pt7b,kUiMuted);
    c.fillRect(24,16,3,14,kUiFg);
    for(int row=0;row<14;row++){int w=row<7?row+1:14-row;c.fillRect(38-w,16+row,w,1,kUiFg);}
    for(int row=0;row<14;row++){int w=row<7?row+1:14-row;c.fillRect(241,16+row,w,1,kUiFg);}
    c.fillRect(252,16,3,14,kUiFg);
    if(s.playing){c.fillRect(241,52,4,16,kUiFg);c.fillRect(251,52,4,16,kUiFg);}
    else for(int row=0;row<16;row++){int w=row<8?row+1:16-row;c.fillRect(244,52+row,w,1,kUiFg);}
    // A fixed text column (24..224), 16px gutter, and control column (240..256).
    std::string rest=s.title[0]?s.title:"Untitled track";
    int lines=0;
    while(!rest.empty()&&lines<3) {
        std::string line;
        if(lines==2){line=ellipsis(rest,FreeSansBold12pt7b,200);rest.clear();}
        else {
            size_t end=0,lastSpace=0;
            while(end<rest.size()&&width(rest.substr(0,end+1),FreeSansBold12pt7b)<=200){if(rest[end]==' ')lastSpace=end;end++;}
            if(end<rest.size()&&lastSpace)end=lastSpace;
            if(!end)end=1;
            line=rest.substr(0,end);rest.erase(0,end);
            while(!rest.empty()&&rest[0]==' ')rest.erase(0,1);
        }
        label(c,line,24,92+lines*28,FreeSansBold12pt7b,kUiFg);lines++;
    }
    if(s.artist[0])label(c,ellipsis(s.artist,FreeSans9pt7b,200),24,92+(lines-1)*28+24,FreeSans9pt7b,kUiMuted);
    if(s.showVolume){c.fillRect(244,84,8,64,kUiPanel);int h=64*s.volume/100;if(h)c.fillRect(244,148-h,8,h,kUiFg);}
    c.fillRect(24,188,232,8,kUiPanel);
    unsigned elapsed=s.elapsed<s.duration?s.elapsed:s.duration;
    int fill=s.duration?(int)((uint64_t)232*elapsed/s.duration):0;
    if(fill)c.fillRect(24,188,fill,8,kUiFg);
    char left[16],right[16];snprintf(left,sizeof(left),"%u:%02u",s.elapsed/60,s.elapsed%60);
    if(s.duration)snprintf(right,sizeof(right),"%u:%02u",s.duration/60,s.duration%60);else strcpy(right,"--:--");
    label(c,left,24,220,FreeSans9pt7b,kUiFg);
    label(c,right,256-width(right,FreeSans9pt7b),220,FreeSans9pt7b,kUiFg);
}
}
