#pragma once
#include <stdint.h>
#include <string.h>
#include "ui/pixel_font.h"
#include "ui/instrument_text.h"
#include "config/ui_theme.h"
namespace ui { namespace pixel_text {
// Existing native pixel font, 6*scale advance. Coordinates are glyph-box top
// left; unsupported bytes use '?'. Canvas supplies fillRect, including clipping.
// shorten requires columns>=3 and output capacity>=columns+1; inputs non-null.
template<class Canvas>
void text(Canvas& c, const char* value, int x, int y, int scale, uint16_t color) {
#ifdef ORIGAMI_INSTRUMENT_FONT
    instrument_text::text(c,value,x,y,instrument_text::cell(scale),color);
#else
    for (const unsigned char* p = (const unsigned char*)value; *p; ++p, x += 6*scale) {
        const unsigned ch = *p >= 32 && *p <= 126 ? *p : '?';
        const auto& glyph = PixelFontGlyphs[ch-32];
        for (int row=0; row<glyph.height; ++row) {
            const unsigned bits = pgm_read_byte(PixelFontBitmaps + glyph.bitmapOffset + row);
            for (int col=0; col<8; ++col)
                if (bits & (128 >> col)) c.fillRect(x+col*scale, y+row*scale, scale, scale, color);
        }
    }
#endif
}

template<class Canvas>
void centered(Canvas& c, const char* value, int y, int scale, uint16_t color=kUiFg, int centerX=140) {
    text(c,value,centerX-((int)strlen(value)*6*scale-scale)/2,y,scale,color);
}
inline void shorten(const char* source, char* output, unsigned columns) {
    const unsigned n = strlen(source);
    if (n<=columns) { memcpy(output,source,n+1); return; }
    memcpy(output,source,columns-3); memcpy(output+columns-3,"...",4);
}
} }
