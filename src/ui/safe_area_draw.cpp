#include "ui/safe_area_draw.h"
#include <string.h>

namespace ui { namespace safe_area {
bool applies(TFT_eSPI& c) { return c.width() == kWidth && c.height() == kHeight; }

namespace {
int16_t paint(TFT_eSPI& c, const char* text, Box requested, uint8_t font, Region region) {
    const Box box = fitBox(requested, region);
    // Bounded stack storage; existing scene captions use the same 160-byte cap.
    char fitted[160];
    const size_t length = strlen(text);
    size_t n = min(int(length), int(sizeof(fitted)-1));
    memcpy(fitted,text,n);
    fitted[n] = 0;
    if (length > n || c.textWidth(fitted,font) > box.w) {
        // Never leave a partial UTF-8 code point before the ASCII ellipsis.
        n = min(int(n), int(sizeof(fitted)-4));
        do {
            while (n && (uint8_t(text[n]) & 0xC0) == 0x80) --n;
            strcpy(fitted+n,"...");
            if (c.textWidth(fitted,font) <= box.w) break;
            if (!n) { fitted[0] = 0; break; }
            --n;
        } while (true);
    }
    const uint8_t oldDatum = c.getTextDatum();
    c.setTextDatum(TL_DATUM);
    const int16_t result = c.drawString(fitted,box.x,box.y,font);
    c.setTextDatum(oldDatum);
    return result;
}
}
int16_t drawString(TFT_eSPI& c, const char* text, int x, int y, uint8_t font, Region region) {
    if (!text) text = "";
    if (!font) font = c.textfont;
    if (!applies(c)) return c.drawString(text,x,y,font);
    const int w = c.textWidth(text,font), h = c.fontHeight(font);
    const uint8_t datum = c.getTextDatum();
    // TFT datum rows: top 0..2, middle 3..5, bottom 6..8, baseline 9..11.
    if (datum % 3 == 1) x -= w/2;
    else if (datum % 3 == 2) x -= w;
    if (datum >= 3 && datum <= 5) y -= h/2;
    else if (datum >= 6) y -= h;
    return paint(c,text,{x,y,max(1,w),h},font,region);
}
int16_t drawLabel(TFT_eSPI& c, const char* text, Box box, uint8_t datum, uint8_t font, Region region) {
    if (!text) text = "";
    if (!font) font = c.textfont;
    const uint8_t oldDatum = c.getTextDatum();
    c.setTextDatum(datum);
    const int w = c.textWidth(text,font), h = c.fontHeight(font);
    int x = box.x, y = box.y;
    if (datum % 3 == 1) x += box.w/2;
    else if (datum % 3 == 2) x += box.w;
    if (datum >= 3 && datum <= 5) y += box.h/2;
    else if (datum >= 6) y += box.h;
    int16_t result;
    if (!applies(c)) result = c.drawString(text,x,y,font);
    else {
        const int actualW = min(w,max(1,box.w));
        if (datum % 3 == 1) x -= actualW/2;
        else if (datum % 3 == 2) x -= actualW;
        if (datum >= 3 && datum <= 5) y -= h/2;
        else if (datum >= 6) y -= h;
        result = paint(c,text,{x,y,max(1,actualW),h},font,region);
    }
    c.setTextDatum(oldDatum);
    return result;
}
int16_t drawCentreString(TFT_eSPI& c, const char* text, int x, int y, uint8_t font) {
    const uint8_t old = c.getTextDatum(); c.setTextDatum(TC_DATUM);
    const int16_t result = drawString(c,text,x,y,font);
    c.setTextDatum(old); return result;
}
int16_t drawRightString(TFT_eSPI& c, const char* text, int x, int y, uint8_t font) {
    const uint8_t old = c.getTextDatum(); c.setTextDatum(TR_DATUM);
    const int16_t result = drawString(c,text,x,y,font);
    c.setTextDatum(old); return result;
}
}}
