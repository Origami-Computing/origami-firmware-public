#include "ui/text_wrap.h"
#include "ui/default_font_metrics.h"
#include "ui/safe_area_draw.h"
#include "ui/pixel_font.h"
#include <cstring>

namespace ui {
namespace text {

// ── helpers ──────────────────────────────────────────────────────────────────

static int lineH(uint8_t sz) { return default_font::height(sz) + (sz >= 2 ? 6 : 3); }

// Advance `p` past one visual word-wrap line of `cpl` chars per line.
// Returns new `p` position. Works for both font families (same break logic).
static const char* advanceLine(const char* p, int cpl) {
    int n = 0, sp = -1;
    while (p[n] && n < cpl) {
        if (p[n] == '\n') { sp = n; break; }
        if (p[n] == ' ')  sp = n;
        n++;
    }
    int pr = (!p[n] || n < cpl) ? n : (sp > 0 ? sp : n);
    if (pr <= 0) {
        if (p[0] == '\n') return p + 1;
        return p;  // stuck
    }
    p += pr;
    if (*p == ' ' || *p == '\n') p++;
    return p;
}

// ── Sized (bitmap font) ──────────────────────────────────────────────────────

int wrappedSized(TFT_eSPI& d, const char* s, int x, int y,
                 int maxW, int maxLines, uint16_t fg, uint16_t bg, uint8_t sz) {
    d.setTextFont(1);
    if (!s || !*s) return y;
    d.setTextColor(fg, bg);
    d.setTextSize(sz);
    const int LH=lineH(sz);
    if (maxW<=0) return y;
    char line[128];
    const char* p=s;
    int linesUsed=0;
    for (; linesUsed<maxLines && *p; ++linesUsed) {
        const auto br=default_font::line(p,maxW,sz);
        if (!br.next) break;
        memcpy(line,p,br.bytes); line[br.bytes]=0;
        d.setTextDatum(TL_DATUM);
        safe_area::drawString(d,line,x,y+linesUsed*LH);
        p+=br.next;
    }
    return y+linesUsed*LH;
}

int wrappedSizedPrefix(TFT_eSPI& d, const char* s, size_t prefixChars,
                       int x, int y, int maxW, int maxLines,
                       uint16_t fg, uint16_t bg, uint8_t sz,
                       char* scratch, size_t scratchCap) {
    if (!scratch || scratchCap == 0)
        return wrappedSized(d, "", x, y, maxW, maxLines, fg, bg, sz);
    if (!s) {
        scratch[0] = '\0';
        return wrappedSized(d, scratch, x, y, maxW, maxLines, fg, bg, sz);
    }
    size_t L = strlen(s);
    if (prefixChars > L) prefixChars = L;
    if (prefixChars >= scratchCap) prefixChars = scratchCap - 1;
    memcpy(scratch, s, prefixChars);
    scratch[prefixChars] = '\0';
    return wrappedSized(d, scratch, x, y, maxW, maxLines, fg, bg, sz);
}

int countWrapped(const char* s, int maxW, uint8_t sz) {
    if (!s || maxW<=0) return 0;
    int lines=0;
    while (*s) {
        const auto br=default_font::line(s,maxW,sz);
        if (!br.next) break;
        s+=br.next; ++lines;
    }
    return lines;
}

const char* skipWrapped(const char* s, int skipLines, int maxW, uint8_t sz) {
    if (!s) return "";
    for (int i=0;i<skipLines && *s && maxW>0;++i) {
        const auto br=default_font::line(s,maxW,sz);
        if (!br.next) break;
        s+=br.next;
    }
    return s;
}

// Preserve the legacy baseline API; Instrument uses shared proportional wrapping.
int wrappedPokemon(TFT_eSPI& d, const char* s, int x, int y,
                   int maxW, int maxLines, uint16_t fg, uint16_t bg, int lineHeight) {
#ifdef ORIGAMI_INSTRUMENT_FONT
    // Preserve callers' legacy baseline while using the default proportional face.
    d.setTextFont(1); d.setTextSize(kPokeSz); d.setTextColor(fg,bg);
    if (!s || maxW<=0) return y;
    const int top=y-7*kPokeSz;
    int lines=0; char line[128];
    while (*s && lines<maxLines) {
        const auto br=default_font::line(s,maxW,kPokeSz);
        if (!br.next) break;
        memcpy(line,s,br.bytes);line[br.bytes]=0;
        d.setTextDatum(TL_DATUM);
        safe_area::drawString(d,line,x,top+lines*lineHeight);
        s+=br.next; ++lines;
    }
    return y+lines*lineHeight;
#else
    d.setFreeFont(&PixelFont);
    if (!s || !*s) return y;
    d.setTextColor(fg, bg);
    d.setTextSize(kPokeSz);
    const int cpl = maxW / kPokeCW;
    if (cpl <= 0) return y;
    char line[128];
    if (cpl >= (int)sizeof(line)) return y;

    const char* p = s;
    int linesUsed = 0;
    for (int l = 0; l < maxLines && *p; l++) {
        int n = 0, sp = -1;
        while (p[n] && n < cpl) {
            if (p[n] == '\n') { sp = n; break; }
            if (p[n] == ' ')  sp = n;
            n++;
        }
        int pr = (!p[n] || n < cpl) ? n : (sp > 0 ? sp : n);
        if (pr <= 0) {
            if (p[0] == '\n') { p++; continue; }
            break;
        }
        if (pr > (int)sizeof(line) - 1) pr = (int)sizeof(line) - 1;
        memcpy(line, p, (size_t)pr);
        line[pr] = '\0';
        // PixelFont uses a baseline cursor; fit its complete ascender/descender
        // cell as a group while retaining the existing wrapping/pagination.
        int cursorX = x, baselineY = y+l*lineHeight;
        if (safe_area::applies(d)) {
            const auto box = safe_area::fitBox({x,baselineY-7*kPokeSz,pr*kPokeCW+2*kPokeSz,9*kPokeSz});
            // Keep the pagination advance, but never paint glyphs past the
            // fitted line width (GFX glyphs extend beyond their advance).
            const int visible = safe_area::max(0,(box.w-2*kPokeSz)/kPokeCW);
            if (visible < pr) line[visible] = '\0';
            cursorX = box.x;
            baselineY = box.y+7*kPokeSz;
        }
        d.setCursor(cursorX, baselineY);
        d.print(line);
        p += pr;
        if (*p == ' ' || *p == '\n') p++;
        linesUsed = l + 1;
    }
    return y + linesUsed * lineHeight;
#endif
}

int wrappedPokemonPrefix(TFT_eSPI& d, const char* s, size_t prefixChars,
                         int x, int y, int maxW, int maxLines,
                         uint16_t fg, uint16_t bg,
                         char* scratch, size_t scratchCap, int lineHeight) {
    if (!scratch || scratchCap == 0)
        return wrappedPokemon(d, "", x, y, maxW, maxLines, fg, bg, lineHeight);
    if (!s) {
        scratch[0] = '\0';
        return wrappedPokemon(d, scratch, x, y, maxW, maxLines, fg, bg, lineHeight);
    }
    size_t L = strlen(s);
    if (prefixChars > L) prefixChars = L;
    if (prefixChars >= scratchCap) prefixChars = scratchCap - 1;
    memcpy(scratch, s, prefixChars);
    scratch[prefixChars] = '\0';
    return wrappedPokemon(d, scratch, x, y, maxW, maxLines, fg, bg, lineHeight);
}

int countPokemon(const char* s, int maxW) {
#ifdef ORIGAMI_INSTRUMENT_FONT
    return countWrapped(s,maxW,kPokeSz);
#else
    if (!s || !*s) return 0;
    const int cpl = maxW / kPokeCW;
    if (cpl <= 0) return 0;
    int lines = 0;
    const char* p = s;
    while (*p) {
        int n = 0, sp = -1;
        while (p[n] && n < cpl) {
            if (p[n] == '\n') { sp = n; break; }
            if (p[n] == ' ')  sp = n;
            n++;
        }
        int pr = (!p[n] || n < cpl) ? n : (sp > 0 ? sp : n);
        if (pr <= 0) {
            if (p[0] == '\n') { p++; continue; }
            break;
        }
        p += pr;
        if (*p == ' ' || *p == '\n') p++;
        lines++;
    }
    return lines;
#endif
}

const char* skipPokemon(const char* s, int skipLines, int maxW) {
#ifdef ORIGAMI_INSTRUMENT_FONT
    return skipWrapped(s,skipLines,maxW,kPokeSz);
#else
    if (!s) return "";
    const int cpl = maxW / kPokeCW;
    if (cpl <= 0 || skipLines <= 0) return s;
    const char* p = s;
    for (int l = 0; l < skipLines && *p; l++) {
        int n = 0, sp = -1;
        while (p[n] && n < cpl) {
            if (p[n] == '\n') { sp = n; break; }
            if (p[n] == ' ')  sp = n;
            n++;
        }
        int pr = (!p[n] || n < cpl) ? n : (sp > 0 ? sp : n);
        if (pr <= 0) {
            if (p[0] == '\n') { p++; continue; }
            break;
        }
        p += pr;
        if (*p == ' ' || *p == '\n') p++;
    }
    return p;
#endif
}

}  // namespace text
}  // namespace ui
