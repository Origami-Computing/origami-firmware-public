#pragma once
// Photos loading screens, drawn with the photos_view primitives so live UI and
// host fixtures produce the same pixels.
//
//  - photosLoader: a small stack of photo cards (cached thumbnails or grey
//    placeholders) that shuffles while Photos waits for the SD card, with
//    "Photos" / "Loading" underneath. stack() repaints only the card region.
//  - videoLoader: the clip's own thumbnail, dimmed, with its name, length and
//    a timeline that fills while the player decodes the first frame.
//    videoBar() repaints only the timeline strip.
#include "ui/photos_view.h"
#include <math.h>
#include <string.h>

namespace ui {
namespace photos_loader {

using photos_view::Bounds;
using photos_view::kBlack;
using photos_view::kWhite;

constexpr int kCardW = 76, kCardH = 56, kCardR = 6;
constexpr int kCardPixels = kCardW * kCardH;
constexpr int kCenterX = 141, kCenterY = 96;   // case-opening center column
// Covers the three resting cards and the path of the card leaving to the right.
constexpr int kStackX = 84, kStackY = 48, kStackW = 196, kStackH = 98;
constexpr uint32_t kShuffleMs = 1500;
constexpr uint32_t kFrameMs = 33;

// Minimal RGB565 surface with the canvas contract photos_view expects.
struct Pixels {
    uint16_t* p;
    int w, h;
    uint16_t readPixel(int x, int y) const { return p[y * w + x]; }
    void drawPixel(int x, int y, uint16_t c) { p[y * w + x] = c; }
    void fillRect(int x, int y, int fw, int fh, uint16_t c) {
        for (int yy = y; yy < y + fh; ++yy)
            for (int xx = x; xx < x + fw; ++xx) p[yy * w + xx] = c;
    }
};

// Grey card with the Photos hills-and-sun mark, for libraries with no thumbnails.
inline void placeholder(uint16_t* out) {
    Pixels px{out, kCardW, kCardH};
    const Bounds b{kCardW, kCardH};
    px.fillRect(0, 0, kCardW, kCardH, photos_view::kSheet);
    const uint16_t ink = photos_view::kIconMuted;
    const float x = 8, y = 6, w = 60, h = 44;
    photos_view::triangle(px, b, x + w * .08f, y + h * .9f, x + w * .38f, y + h * .42f, x + w * .66f, y + h * .9f, ink);
    photos_view::triangle(px, b, x + w * .5f, y + h * .9f, x + w * .7f, y + h * .58f, x + w * .92f, y + h * .9f, ink);
    photos_view::disc(px, b, x + w * .68f, y + h * .3f, 4.f, ink);
}

inline float ease(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t < .5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
}

// Signed distance from a card-centred point to the card's rounded outline.
inline float cardDistance(float u, float v) {
    const float qx = fabsf(u) - (kCardW / 2.f - kCardR), qy = fabsf(v) - (kCardH / 2.f - kCardR);
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    const float inside = qx > qy ? qx : qy;
    return hypotf(ox, oy) + (inside < 0 ? inside : 0) - kCardR;
}

// One card rotated about its centre. dim 0..255 darkens; alpha 0..255 fades.
template <class C>
void card(C& c, Bounds s, const uint16_t* px, float cx, float cy, float deg, int dim, int alpha) {
    if (alpha <= 0) return;
    const float a = deg * 3.14159265f / 180.f, cs = cosf(a), sn = sinf(a);
    const float ex = kCardW / 2.f * fabsf(cs) + kCardH / 2.f * fabsf(sn) + 1;
    const float ey = kCardW / 2.f * fabsf(sn) + kCardH / 2.f * fabsf(cs) + 1;
    // Interior pixels (well inside the edge and corners) skip the distance
    // field and, when opaque, the read-back blend. Only the rim is anti-aliased.
    constexpr float hw = kCardW / 2.f, hh = kCardH / 2.f, rim = 1.5f;
    const int y0 = std::max(0, int(cy - ey)), y1 = std::min(s.h - 1, int(cy + ey));
    const int x0 = std::max(0, int(cx - ex)), x1 = std::min(s.w - 1, int(cx + ex));
    for (int y = y0; y <= y1; ++y) {
        const float dy = y + .5f - cy;
        float u = (x0 + .5f - cx) * cs + dy * sn, v = -(x0 + .5f - cx) * sn + dy * cs;
        for (int x = x0; x <= x1; ++x, u += cs, v -= sn) {
            const float au = fabsf(u), av = fabsf(v);
            if (au >= hw + .5f || av >= hh + .5f) continue;
            const bool interior = au < hw - rim && av < hh - rim &&
                                  (au < hw - kCardR || av < hh - kCardR);
            const float d = interior ? -2.f : cardDistance(u, v);
            if (d >= .5f) continue;
            int sx = int(u + hw), sy = int(v + hh);
            sx = sx < 0 ? 0 : sx >= kCardW ? kCardW - 1 : sx;
            sy = sy < 0 ? 0 : sy >= kCardH ? kCardH - 1 : sy;
            uint16_t ink = px[sy * kCardW + sx];
            if (d > -rim) ink = photos_view::mix(kWhite, ink, 51);   // 20% hairline edge
            if (dim > 0) ink = photos_view::mix(kBlack, ink, dim);
            const float cov = d < -.5f ? 1.f : .5f - d;
            if (cov >= 1.f && alpha >= 255) c.drawPixel(x, y, ink);
            else photos_view::blend(c, s, x, y, ink, int(cov * alpha));
        }
    }
}

// Cards newest first. count >= 3 (callers pad with the placeholder).
struct Deck {
    const uint16_t* cards[8];
    int count;
};

// Repaints the card region for time ms. The front card slides off to the right
// while the next one moves up and a new one fades in at the back.
template <class C> void stack(C& c, Bounds s, const Deck& deck, uint32_t ms) {
    photos_view::fill(c, s, kStackX, kStackY, kStackW, kStackH, kBlack);
    if (deck.count <= 0) return;
    struct Slot { float dx, dy, deg, dim; };
    static constexpr Slot kSlots[3] = {{-12, -9, -7, 128}, {-5, -4, -3, 64}, {0, 0, 0, 0}};
    const uint32_t n = ms / kShuffleMs;
    const float ph = ease(float(ms % kShuffleMs) / (kShuffleMs * .7f));
    struct Item { float pos; int photo; } items[3];
    for (int i = 0; i < 3; ++i) {
        const int slot = int((i + n) % 3);
        items[i] = {slot + ph, int((n + 2 - slot) % uint32_t(deck.count))};
    }
    // Back to front.
    for (int i = 1; i < 3; ++i)
        for (int j = i; j > 0 && items[j - 1].pos > items[j].pos; --j) {
            const Item t = items[j]; items[j] = items[j - 1]; items[j - 1] = t;
        }
    for (const Item& it : items) {
        float dx, dy, deg, dim, alpha = 1;
        if (it.pos >= 2) {
            const float q = it.pos - 2;
            dx = q * 90; dy = -q * 6; deg = q * 14; dim = 0; alpha = 1 - q;
        } else {
            const int lo = int(it.pos);
            const float q = it.pos - lo;
            const Slot& a = kSlots[lo]; const Slot& b = kSlots[lo + 1];
            dx = a.dx + (b.dx - a.dx) * q; dy = a.dy + (b.dy - a.dy) * q;
            deg = a.deg + (b.deg - a.deg) * q; dim = a.dim + (b.dim - a.dim) * q;
            if (it.pos < 1) alpha = it.pos;
        }
        card(c, s, deck.cards[it.photo], kCenterX + dx, kCenterY + dy, deg, int(dim), int(alpha * 255));
    }
}

template <class C> void photosLoader(C& c, Bounds s, const Deck& deck, uint32_t ms) {
    c.fillRect(0, 0, s.w, s.h, kBlack);
    stack(c, s, deck, ms);
    photos_view::textCenter(c, s, "Photos", kCenterX, 151, 24, kWhite);
    photos_view::textCenter(c, s, "Loading", kCenterX, 184, 12, photos_view::kMuted);
}

// --- Video ------------------------------------------------------------------
// The same title card playback opens with (name, "Starting video", length,
// knob at 0), over the clip's own poster, so the first frame takes over without
// a jump. While the player starts, a lighter buffered fill grows along the
// track behind the knob.

namespace pv = photos_view;
constexpr int kStripY = pv::kCardTrackY - 8, kStripH = 18;   // track, knob and shadows
constexpr int kStripPixels = 280 * kStripH;

// Saves / restores the poster pixels under the track so frames repaint cleanly.
template <class C> void saveStrip(C& c, Bounds s, uint16_t* out) {
    for (int y = 0; y < kStripH; ++y)
        for (int x = 0; x < s.w && x < 280; ++x) out[y * 280 + x] = c.readPixel(x, kStripY + y);
}
template <class C> void restoreStrip(C& c, Bounds s, const uint16_t* in) {
    for (int y = 0; y < kStripH; ++y)
        for (int x = 0; x < s.w && x < 280; ++x) c.drawPixel(x, kStripY + y, in[y * 280 + x]);
}

template <class C> void videoTrack(C& c, Bounds s, uint32_t ms) {
    const int x = pv::kCardLeft, y = pv::kCardTrackY, w = pv::kCardRight - pv::kCardLeft;
    // Eases toward 90% while the first frame decodes; playback replaces it.
    const int buffered = int(w * .9f * (1 - expf(-float(ms) / 1500.f)));
    pv::roundRect(c, s, x, y + 1, w, 4, 2, kBlack, 140);                 // shadow
    pv::roundRect(c, s, x, y, w, 4, 2, pv::rgb(120, 120, 124));          // track
    if (buffered > 4) pv::roundRect(c, s, x, y, buffered, 4, 2, pv::rgb(185, 185, 190));
    pv::disc(c, s, x + .5f, y + 3, 6, kBlack, 120);                      // knob shadow
    pv::disc(c, s, x + .5f, y + 2, 6, kWhite);                           // knob
}

// Caller draws the poster (cover crop) or black first. Name has no extension.
// strip (optional, kStripPixels) receives the poster under the track for videoBar().
template <class C>
void videoLoader(C& c, Bounds s, const char* name, const char* duration, uint32_t ms, uint16_t* strip = nullptr) {
    if (strip) saveStrip(c, s, strip);
    pv::videoCardSolid(c, s, pv::VideoCard{name, "Starting video", duration, 0});
    videoTrack(c, s, ms);
}

// Later frames: only the track strip changes.
template <class C> void videoBar(C& c, Bounds s, const uint16_t* strip, uint32_t ms) {
    if (strip) restoreStrip(c, s, strip);
    else pv::fill(c, s, 0, kStripY, s.w, kStripH, kBlack);
    videoTrack(c, s, ms);
}

}  // namespace photos_loader
}  // namespace ui
