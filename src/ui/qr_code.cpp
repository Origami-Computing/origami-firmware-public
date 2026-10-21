#include "ui/qr_code.h"
#include "ui/safe_area_draw.h"

namespace ui {

void renderQrCode(TFT_eSPI& tft, QRCode* qr, int x, int y, int px, uint16_t fg, uint16_t bg) {
    if (!qr || px <= 0) return;
    if (safe_area::applies(tft)) {
        auto box = safe_area::fitBox({x-3,y-3,qr->size*px+6,qr->size*px+6});
        px = min(px,(min(box.w,box.h)-6)/qr->size);
        if (px < 1) return;
        box = safe_area::fitBox({x-3,y-3,qr->size*px+6,qr->size*px+6});
        x = box.x+3; y = box.y+3;
    }
    const int totalPx = qr->size * px;
    tft.fillRect(x - 3, y - 3, totalPx + 6, totalPx + 6, bg);
    for (int my = 0; my < qr->size; my++) {
        for (int mx = 0; mx < qr->size; mx++) {
            const uint16_t col = qrcode_getModule(qr, (uint8_t)mx, (uint8_t)my) ? fg : bg;
            tft.fillRect(x + mx * px, y + my * px, px, px, col);
        }
    }
}

}  // namespace ui
