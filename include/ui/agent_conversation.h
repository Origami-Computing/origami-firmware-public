#pragma once
#include <cstdio>
#include "config/ui_theme.h"
#include "ui/proportional_text.h"
#include "ui/eye_rig_raster.h"
#include "ui/scrollbar_layout.h"

// Shared painter for the Kami voice conversation (see docs/voice-turns.md).
// Full-panel geometry: the status bar is hidden outside Home. Kami's eyes stay
// centred while you talk; your words sit on one line under them and slide left,
// fading at the left edge, once they outgrow the line. When Kami answers, the
// eyes ease up and shrink so the reply gets several lines.
// Pure drawing: no app state, I/O or owned surface. UI task only.
namespace ui { namespace agent_conversation {

constexpr int x = 24, width = 232;          // reply text column
constexpr int replyY = 84, pitch = 24, replyRows = 5;
constexpr int lineBaseline = 216;           // your one line / hints, below the Home eye band (≤194)
constexpr int subBaseline = 236;            // agent mode: your live line under a big answer
constexpr int textBandTop = 196;            // text layer: rows the eye renderer never repaints
constexpr int fadeFrom = 16, fadeTo = 72;   // left-edge fade of an overflowing line
constexpr uint16_t kInterim = 0x8C71;       // interim and sent words
constexpr uint16_t kLive = 0xF945;          // Kami red: agent-mode microphone marker

inline const char* skip(const char* text, int lines) {
    while (*text && lines-- > 0) { auto l = proportional::line(text, width, 18); if (!l.next) break; text += l.next; }
    return text;
}
inline int count(const char* text) {
    int n = 0; while (*text) { auto l = proportional::line(text, width, 18); if (!l.next) break; text += l.next; ++n; } return n;
}

// Eye geometry between the listening size (t=0) and the reply size (t=1).
struct EyeFrame { float t = 0, open = 1, offsetX = 0, offsetY = 0; };
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float eyeNominal(float t) { return lerp(64.f, 26.f, t); }
inline float eyeCenterY(float t) { return lerp(98.f, 40.f, t); }

template<class C> void clear(C& c) { c.fillRect(0, 0, 280, 240, kUiBg); }

template<class C> void eyes(C& c, const eye_rig::Pose& pose, const EyeFrame& f) {
    const float nominal = eyeNominal(f.t);
    const int h = (int)(nominal * 91.f / 67.f * 1.3f) + 8;
    const int top = (int)eyeCenterY(f.t) - h / 2;
    eye_rig::drawRaster(c, 0, top, 280, h, pose, f.open, kUiBg, nominal, f.offsetX, f.offsetY,
                        false, 255, 0, -1, eye_rig::kHomeSpacing, 1);
}

// One line at the line baseline with per-pixel ink. Characters from `greyFrom`
// on use kInterim; pixels left of fadeTo fade into the background.
template<class C> void fadedRun(C& c, const char* s, int left, int px, uint16_t ink, const char* greyFrom, bool fade, int baseline = lineBaseline) {
    const auto& f = proportional::font(px);
    unsigned advance = 0;
    for (const char* p = s; *p; ++p) {
        const unsigned char ch = (unsigned char)*p < 32 ? ' ' : (unsigned char)*p;
        const auto& g = proportional::glyph(ch, px);
        const int gx = left + int((advance + 32) / 64) + g.x;
        advance += g.advance;
        if (gx + g.w <= 0 || ch == ' ') continue;
        if (gx >= 280) break;
        const uint16_t color = greyFrom && p >= greyFrom ? kInterim : ink;
        for (int row = 0; row < g.h; ++row) for (int col = 0; col < g.w; ++col) {
            const int px0 = gx + col;
            if (px0 < 0 || px0 >= 280) continue;
            unsigned a = f.pixels[g.offset + row * g.w + col];
            if (!a) continue;
            if (fade && px0 < fadeTo) a = px0 <= fadeFrom ? 0 : a * unsigned(px0 - fadeFrom) / unsigned(fadeTo - fadeFrom);
            if (a) c.fillRect(px0, baseline + g.y + row, 1, 1, proportional::blend(color, kUiBg, a));
        }
    }
}

// Your words: centred while they fit, then anchored right and faded at the left.
// interim=true shows the last (still changing) word in grey; dim=true is the
// sent state while Kami thinks.
template<class C> void line(C& c, const char* text, bool interim, bool dim) {
    if (!text || !*text) return;
    const int w = proportional::width(text, 18);
    const char* greyFrom = nullptr;
    if (interim) { const char* sp = strrchr(text, ' '); greyFrom = sp ? sp + 1 : text; }
    const bool fits = w <= width;
    const int left = fits ? 140 - w / 2 : x + width - w;
    fadedRun(c, text, left, 18, dim ? kInterim : kUiFg, dim ? nullptr : greyFrom, !fits);
}

// A short answer that fits one line stays big under the listening-size eyes.
constexpr int kAnswerPx = 24, kAnswerFallbackPx = 20;
inline int answerPx(const char* text) {
    if (!text || !*text || strchr(text, '\n')) return 0;
    if (proportional::width(text, kAnswerPx) <= width) return kAnswerPx;
    if (proportional::width(text, kAnswerFallbackPx) <= width) return kAnswerFallbackPx;
    return 0;
}
template<class C> void answer(C& c, const char* text, int px, int baseline = lineBaseline) {
    const int w = proportional::width(text, px);
    proportional::atBaseline(c, text, 140 - w / 2, baseline, px, kUiFg, kUiBg);
}

// Agent mode under a big answer: your live words (or the hint) small and grey.
template<class C> void subLine(C& c, const char* text) {
    if (!text || !*text) return;
    const int w = proportional::width(text, 14);
    const bool fits = w <= width;
    fadedRun(c, text, fits ? 140 - w / 2 : x + width - w, 14, kInterim, nullptr, !fits, subBaseline);
}

template<class C> void hint(C& c, const char* text) {
    const int w = proportional::width(text, 16);
    proportional::atBaseline(c, text, 140 - w / 2, lineBaseline, 16, kInterim, kUiBg);
}

// Multi-message replies: one message per screen, "2 of 3" in the top-right corner.
template<class C> void pageMarker(C& c, int index, int total) {
    if (total < 2) return;
    char label[16]; snprintf(label, sizeof(label), "%d of %d", index + 1, total);
    const int w = proportional::width(label, 12);
    proportional::atBaseline(c, label, 256 - w, 22, 12, kInterim, kUiBg);
}

// Agent mode: the microphone stays open; the marker sits left of your line.
template<class C> void liveDot(C& c, int baseline = lineBaseline) { c.fillCircle(14, baseline - 6, 4, kLive); }

// Kami's reply: measured lines from `scroll`, with the shared scrollbar geometry.
template<class C> void reply(C& c, const char* text, int scroll, int rows) {
    proportional::paragraph(c, skip(text, scroll), x, replyY, width, rows, 18, pitch, kUiFg, kUiBg);
    const int total = count(text);
    if (total > rows) {
        const int trackY = replyY - 4, trackH = rows * pitch;
        const auto thumb = scrollbarThumb(trackY, trackH, rows, total, scroll);
        c.fillRect(266, trackY, 3, trackH, kUiScrollTrack);
        c.fillRect(266, thumb.y, 3, thumb.height, kUiScrollThumb);
    }
}

// Documents keep their existing sheet (out of scope for the voice-turn redesign).
template<class C> void document(C& canvas,const char* title,const char* value,int scroll,bool truncated,const char* footer=nullptr,bool preview=false) {
    canvas.fillRect(0,80,280,160,0xffff);
    canvas.drawRect(8,82,264,158,0xc618);
    canvas.fillRect(124,87,32,2,0xc618);
    proportional::paragraph(canvas,title,24,99,232,1,18,22,0x18c3);
    proportional::text(canvas,preview?"Writing...":truncated?"Saved - more below":"Saved",24,126,12,0x630c);
    proportional::paragraph(canvas,skip(value,scroll),24,148,232,footer?2:4,18,22,0x18c3);
    if(footer) proportional::paragraph(canvas,footer,24,198,232,2,14,18,0x18c3);
}
template<class C> void documentEyes(C& canvas,const eye_rig::Pose& pose,float open=1) {
    canvas.fillRect(0,0,280,80,0xffff);
    eye_rig::drawRaster(canvas,110,40,60,36,pose,open,0xffff,22,0,0,false,255,0,-1,eye_rig::kHomeSpacing,1);
}
}}
