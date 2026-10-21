#pragma once

#include <TB_TFT_eSPI.h>
#include <stdint.h>
#include "qrcode.h"

namespace ui {

/**
 * Render a pre-encoded QR code onto `tft` as pixel-aligned filled squares.
 *
 * `qr`      — encoded QRCode struct (from qrcode_initText / qrcode_initBytes).
 * `x`, `y`  — top-left corner of the QR matrix in pixels.
 * `px`      — size of each QR module in screen pixels (1 = 1:1, 3 = 3×3 per module, etc.).
 * `fg`      — module colour (dark squares). Typically kUiFg.
 * `bg`      — background colour (light squares + quiet-zone border). Typically kUiBg.
 *
 * Draws a 3-pixel quiet-zone border around the matrix in `bg`, then fills each
 * module cell with `fg` or `bg`.  Total rendered area is (qr.size * px + 6) px square.
 */
void renderQrCode(TFT_eSPI& tft, QRCode* qr, int x, int y, int px, uint16_t fg, uint16_t bg);

}  // namespace ui
