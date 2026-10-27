#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <TB_TFT_eSPI.h>

namespace hal {
namespace display {

// Landscape dimensions (application coordinate space)
static constexpr int kCanvasW = 280;
static constexpr int kCanvasH = 240;

// Common RGB565 colors (avoid including TFT driver in app)
static constexpr uint16_t COLOR_BLACK   = 0x0000;
static constexpr uint16_t COLOR_WHITE   = 0xFFFF;
static constexpr uint16_t COLOR_RED     = 0xF800;
static constexpr uint16_t COLOR_GREEN   = 0x07E0;
static constexpr uint16_t COLOR_BLUE    = 0x001F;
static constexpr uint16_t COLOR_CYAN    = 0x07FF;
static constexpr uint16_t COLOR_YELLOW  = 0xFFE0;
static constexpr uint16_t COLOR_MAGENTA = 0xF81F;
static constexpr uint16_t COLOR_HEADER  = 0x1082;

bool init();

/**
 * Force a real controller reset on the reworked rev-4 board (J5.4 -> GPIO45).
 * Holds hardware reset and SPI low while cycling panel power, then TFT_eSPI
 * pulses GPIO45 and sends the controller initialization sequence.
 */
bool coldResetAndInit(uint16_t offMs = 750, uint16_t settleMs = 150);
TFT_eSPI& tft();
TFT_eSPI& directTft();
SPIClass& spi();

void setCanvasRedirect(bool on);

/**
 * Optimized render mode for apps that share the SPI bus with SD/I2S/camera
 * DMA (MP3 now-playing, voice recorder, camera). Instead of flushing a full
 * 134 KB canvas frame on every tick, these apps write small targeted pixel
 * regions directly to the panel — only the bytes that changed.
 *
 * enterOptimizedRender() — bypasses canvas flush, puts panel in landscape
 *                          rotation 3 for existing direct pixel-region draws.
 * exitOptimizedRender()  — restores canvas pipeline + marks full frame dirty.
 */
void enterOptimizedRender();
void exitOptimizedRender();
bool isOptimizedRender();

// ---------------------------------------------------------------------------
// Framebuffer pipeline (tear-free rendering)
// ---------------------------------------------------------------------------

/** PSRAM-backed landscape framebuffer (280x240). Draw here instead of tft(). */
TFT_eSprite& canvas();

/** Mark a horizontal band as needing flush. */
void markDirty(int y, int h);
/** UI-task invalidation in canvas coordinates; clips to the canvas. */
void markDirtyRect(int x, int y, int w, int h);
/** Mark entire canvas dirty. */
void markAllDirty();

/**
 * Transpose dirty rectangles from landscape to portrait, acquire SPI, wait for
 * TE, then transfer the composed regions. UI task owns drawing and invalidation.
 */
// One UI-task observer, invoked after successful canvas SPI transfer and unlock.
// Bounds are canvas rows [top,bottom); failed/skipped flushes never notify.
void setFlushObserver(void (*observer)(uint32_t completedMs, int top, int bottom));
void flush();
// UI-thread callback between long flush phases, outside the SPI lock. Used by
// live voice to drain its short DMA ring during full-screen animations.
void setFlushService(void (*service)());

/** UI-task counters for completed framebuffer transfers; direct-panel work is excluded.
 * Unsigned deltas handle wrap. Read on the UI task alongside flush(). */
struct FlushStats { uint32_t frames, pixels, elapsedUs; };
FlushStats flushStats();

/** Microseconds taken by the last flush() call. */
uint32_t lastFlushUs();
/** Approximate frames-per-second (exponential moving average). */
float currentFps();

/** Number of rising edges observed on the panel TE output since display init. */
uint32_t teEdgeCount();

// ---------------------------------------------------------------------------
// Legacy API (escape hatch during migration)
// ---------------------------------------------------------------------------

void setEnabled(bool on);
void setBacklight(bool on);
/**
 * Soft-start the backlight: PWM duty 0->100% over durationMs, then hand the pin
 * back to plain digital HIGH (so setBacklight()/sleep paths stay digital). Used
 * at bringup to bound backlight-FET (Q2) inrush, which otherwise sags +3V3 and
 * can time out the shared I2C bus (see board_bringup / hal_io 263 mitigation).
 */
void rampBacklight(uint16_t durationMs);
void setChipSelect(bool selected);
bool chipSelectLevel();
/** Deselect both LCD and SD from the shared SPI bus before SD card access. */
void releaseSpiForSd();
/** Reassert panel SPI settings and schedule a repaint after a failed SD probe. */
void recoverAfterSharedSpiFailure();
void freeze(bool on);
bool frozen();

void fixColorInversion();
bool waitForVSync();

void fillScreen(uint16_t color);
void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
/** Clear canvas for app redraw without erasing the status-bar column (transpose-safe). */
void clearContentArea(uint16_t color);

void setTextColor(uint16_t fg, uint16_t bg);
void setTextSize(uint8_t size);
void setCursor(int16_t x, int16_t y);
void print(const char* s);
void print(unsigned long n);
void printf(const char* fmt, ...);

bool drawJpegFrame(const uint8_t* buf, size_t len, int screenW, int screenH);
void drawImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* rgb565Buffer);

/**
 * Decode a JPEG file from SD and scale it to fit within (maxW × maxH) at
 * (dstX, dstY), centered, preserving aspect ratio. Never upscales.
 * Writes into tft() — the canvas sprite when the canvas pipeline is active.
 * Falls back to center-crop at native resolution if PSRAM is unavailable.
 */
enum class ImageFit { Contain, Cover };
// Cover also upscales small captures, crops centrally and requires a viewport-
// sized PSRAM surface; allocation failure returns false instead of distorting.
bool drawJpegFileScaled(const char* path,
                        int32_t dstX, int32_t dstY,
                        int32_t maxW, int32_t maxH, ImageFit fit=ImageFit::Contain);

// ---------------------------------------------------------------------------
// Direct-draw SPI mutex bracket
//
// Direct-panel apps (MP3 now-playing, camera, voice recorder) write to the TFT
// hardware outside the canvas pipeline.  SD card reads in the decode task run
// on the same SPI bus.  Without coordination, SD and TFT transactions interleave
// and corrupt each other, causing intermittent screen render freezes.
//
// Call beginDirectDraw() before a sequence of TFT primitive calls (fillRect,
// drawString, etc.) and endDirectDraw() after.  This holds the shared SPI mutex
// for the entire sequence so the SD task waits rather than corrupting the bus.
//
// Group logically related draws into one begin/end pair to minimise lock
// granularity (e.g. the entire spectrum bar repaint is one pair, not one per bar).
// ---------------------------------------------------------------------------
void beginDirectDraw();
void endDirectDraw();

}  // namespace display
}  // namespace hal
