#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(ORIGAMI_SOFT_BLOCK_DISPLAY) || defined(ORIGAMI_INSTRUMENT_FONT)
#include <Fonts/FlashFont.h>
#endif
namespace ui { namespace default_font {
inline int height(uint8_t size) {
#if defined(ORIGAMI_SOFT_BLOCK_DISPLAY) || defined(ORIGAMI_INSTRUMENT_FONT)
    return flash_font::height(size);
#else
    return 8*size;
#endif
}
inline int advance(uint8_t size, uint16_t ch) {
#if defined(ORIGAMI_SOFT_BLOCK_DISPLAY) || defined(ORIGAMI_INSTRUMENT_FONT)
    return flash_font::advance(size,ch);
#else
    return 6*size;
#endif
}
// The draft covers ASCII. Unsupported UTF-8 code points occupy one '?' glyph.
inline size_t character(const char* s, uint16_t& ch) {
    const auto c=(uint8_t)*s; ch=c;
    if (c<128) return 1;
    size_t n=c>=0xF0 ? 4 : c>=0xE0 ? 3 : c>=0xC0 ? 2 : 1;
    for (size_t i=1;i<n;++i) if (((uint8_t)s[i]&0xC0)!=0x80) { n=i; break; }
    ch='?'; return n;
}
inline int width(const char* s, uint8_t size) {
    int result=0;
    while (*s) { uint16_t ch; s+=character(s,ch); result+=advance(size,ch); }
    return result;
}
// Draw, count and skip all use this bounded line break, including blank lines.
struct Line { size_t bytes,next; };
inline Line line(const char* s, int maxW, uint8_t size) {
    size_t n=0,space=0; int used=0;
    while (s[n] && s[n]!='\n') {
        uint16_t ch; const size_t bytes=character(s+n,ch);
        const int a=advance(size,ch);
        if (used+a>maxW || n+bytes>127) break;
        if (ch==' ') space=n;
        n+=bytes; used+=a;
    }
    if (s[n] && s[n]!='\n' && space>0) n=space;
    return {n,n+((s[n]==' ' || s[n]=='\n') ? 1 : 0)};
}
} }
