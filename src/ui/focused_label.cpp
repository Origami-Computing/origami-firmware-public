#include "ui/focused_label.h"
#include "ui/safe_area_draw.h"
#include "hal/hal_display.h"
#include <cstring>

namespace ui { namespace focused_label {
namespace {
TFT_eSPI* target = nullptr;
const void* owner = nullptr;
char label[160] = {};
int x, y, w, h, overflow, lastOffset = -1;
uint16_t fg, bg;
uint8_t font, size;
uint32_t now, started;
bool painted = false, seen = false;
}
void reset() { label[0] = 0; lastOffset = -1; owner = nullptr; target = nullptr; }
void restart(uint32_t time) { started=time; lastOffset=-1; }
void beginFrame(TFT_eSPI& surface, const void* screen, uint32_t time) {
    if (owner != screen) reset();
    owner = screen; target = &surface; now = time; painted = seen = false;
}
void contentPainted(TFT_eSPI& surface) { if (target == &surface) painted = true; }
void observe(TFT_eSPI& surface, const char* text, int px, int py, int width, int height,
             uint16_t foreground, uint16_t background, uint8_t face, uint8_t scale) {
    if (target != &surface) return;
    seen = true;
    if (!text) text = "";
    const auto box = safe_area::applies(surface)
        ? safe_area::fitBox({px,py,width,height}) : safe_area::Box{px,py,width,height};
    // Match the scene's existing bounded caption contract, preserving UTF-8.
    char copy[sizeof(label)];
    size_t n = strlen(text);
    if (n >= sizeof(copy)) {
        n = sizeof(copy)-1;
        while (n && (uint8_t(text[n]) & 0xc0) == 0x80) --n;
    }
    memcpy(copy,text,n); copy[n]=0;
    if (strcmp(label,copy) || x!=box.x || y!=box.y || w!=box.w || h!=box.h ||
        font!=face || size!=scale) {
        memcpy(label,copy,n+1); started=now; lastOffset=-1;
    }
    x=box.x; y=box.y; w=box.w; h=box.h;
    fg=foreground; bg=background; font=face; size=scale;
    surface.setTextFont(font); surface.setTextSize(size);
    overflow=surface.textWidth(label,font)-w;
    // The underlying row was repainted, so restore the moving text this frame.
    lastOffset=-1;
}
bool finishFrame(TFT_eSPI& surface) {
    if (target != &surface) return false;
    target=nullptr;
    if (painted && !seen) label[0]=0;
    if (!label[0] || overflow<=0 || w<=0 || h<=0) return false;
    const int next=offset(now-started,overflow);
    if (next==lastOffset) return false;
    lastOffset=next;
    // Called after the app viewport is reset, before overlays/chrome. Clip only
    // the caption: icons, radio dots and chevrons never move or get overpainted.
    surface.setViewport(x,y,w,h,false);
    surface.fillRect(x,y,w,h,bg);
    surface.setTextFont(font); surface.setTextSize(size);
    surface.setTextDatum(TL_DATUM); surface.setTextColor(fg,bg);
    surface.drawString(label,x-next,y,font);
    surface.resetViewport();
    hal::display::markDirty(y,h);
    return true;
}
} }
