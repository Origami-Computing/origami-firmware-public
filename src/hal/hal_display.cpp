#include "hal/hal_display.h"
#include "core/image_cover.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <SD.h>
#include <TB_TFT_eSPI.h>
#include <JPEGDecoder.h>
#include <esp_heap_caps.h>
#include "Pins.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "ui/screen_chrome.h"
#include "hal/hal_bus.h"
#include "hal/hal_io.h"
#include "hal/display_rotate.h"
#include "hal/display_dirty.h"

namespace hal {
namespace display {

static TFT_eSPI s_tft;
static bool s_frozen = false;
static bool s_pinsReady = false;

static volatile bool s_vsyncFlag = false;
static volatile uint32_t s_teEdgeCount = 0;

static void IRAM_ATTR teISR() {
  s_vsyncFlag = true;
  s_teEdgeCount++;
}

static void ensureControlPins() {
  if (s_pinsReady) {
    return;
  }
  pinMode(LCD_CS_PIN, OUTPUT);
  pinMode(LCD_RESET_PIN, OUTPUT);
  // GPIO45 has a board-level 100k pulldown. Keep the newly wired active-low
  // panel reset asserted until DISP.EN is stable and TFT_eSPI performs its
  // documented HIGH->LOW->HIGH reset pulse.
  digitalWrite(LCD_RESET_PIN, LOW);
  // rev4: LCD_ENABLE_PIN / LCD_BACKLIGHT_PIN are -1 (on the expander). Only drive
  // real ESP GPIOs here; the expander lines are driven via hal::io in
  // setEnabled()/setBacklight().
#if LCD_ENABLE_PIN >= 0
  pinMode(LCD_ENABLE_PIN, OUTPUT);
#endif
#if LCD_BACKLIGHT_PIN >= 0
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
#endif
  s_pinsReady = true;
}

// ---------------------------------------------------------------------------
// Framebuffer pipeline state
// ---------------------------------------------------------------------------
// Tear-free path per docs/display_rendering.md:
//   - Draw into a 280x240 landscape canvas (PSRAM sprite).
//   - setRotation(0) on the panel (no MADCTL MV bit).
//   - Software-transpose canvas -> 240x280 portrait rot buffer.
//   - TE wait, then pushImage to SPI (write direction matches panel scan).
// Hardware rotation 3 (landscape MADCTL) writes perpendicular to the panel
// scan and produces diagonal tearing that TE sync cannot fix.

static TFT_eSprite s_canvas(&s_tft);
static uint16_t*   s_rotBuf     = nullptr;  // 240×280 portrait frame in PSRAM
static bool        s_canvasReady = false;
static volatile bool s_canvasRedirect = true;
static uint32_t    s_lastFlushUs = 0;
static FlushStats s_flushStats{};
static float       s_fps        = 0.0f;
static uint32_t    s_lastFlushTimeUs = 0;

static DirtyBands s_dirty;

static void initFramebuffer() {
  if (s_canvasReady) return;
  s_canvas.setColorDepth(16);
  void* ptr = s_canvas.createSprite(kCanvasW, kCanvasH);
  if (!ptr) {
    Serial.println("[display] WARN: canvas sprite alloc failed");
    s_canvasReady = false;
    return;
  }

  s_rotBuf = (uint16_t*)heap_caps_calloc(
      (size_t)kCanvasH * kCanvasW, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!s_rotBuf) {
    Serial.println("[display] WARN: rotated buffer alloc failed");
    s_canvas.deleteSprite();
    s_canvasReady = false;
    return;
  }

  s_canvasReady = true;
  s_dirty.add(0,0,kCanvasW,kCanvasH);  // all dirty initially
  Serial.printf("[display] Framebuffer ready: canvas %dx%d + rotBuf in PSRAM\n",
                kCanvasW, kCanvasH);
}


bool init() {
  pinMode(LCD_CS_PIN, OUTPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(LCD_CS_PIN, HIGH);
  digitalWrite(SD_CS, HIGH);

  ensureControlPins();
  s_tft.init();
  s_tft.setRotation(0);
  // The backlight is still off, but initialize panel RAM to black so an early
  // light-up can never expose a white controller-default frame.
  s_tft.fillScreen(TFT_BLACK);

  // PORCTRL — 12-line front/back porch (ST7789 default).
  s_tft.writecommand(0xB2);
  s_tft.writedata(0x0C);
  s_tft.writedata(0x0C);
  s_tft.writedata(0x00);
  s_tft.writedata(0x33);
  s_tft.writedata(0x33);

  // FRCTR2 = 0x13 (~53Hz) — slows panel scan so full-frame SPI push
  // completes before the scan catches up.
  s_tft.writecommand(0xC6);
  s_tft.writedata(0x13);

  // TE (tearing effect) output on ST7789 — pulses HIGH during vblank.
  s_tft.writecommand(0x35);
  s_tft.writedata(0x00);
  // Keep TE defined when the display/FPC is absent. The disconnected bare-board
  // input otherwise floats and can generate hundreds of thousands of bogus
  // rising-edge interrupts. The panel's driven TE output easily overrides the
  // ESP32's weak internal pulldown when connected.
  pinMode(LCD_TEAR_PIN, INPUT_PULLDOWN);
  s_vsyncFlag = false;
  s_teEdgeCount = 0;
  attachInterrupt(digitalPinToInterrupt(LCD_TEAR_PIN), teISR, RISING);

  initFramebuffer();

  return true;
}

bool coldResetAndInit(uint16_t offMs, uint16_t settleMs) {
  // Rev-4 rework wires the ST7789 active-low reset (J5.4) to GPIO45. Assert it
  // before removing panel power and keep it low through rail stabilization.
  // SPI outputs are also held low to prevent parasitic panel powering.
  ensureControlPins();
  setBacklight(false);
  digitalWrite(LCD_RESET_PIN, LOW);
  setEnabled(false);

  pinMode(LCD_CS_PIN, OUTPUT);
  pinMode(LCD_DC_PIN, OUTPUT);
  pinMode(SPI_MOSI, OUTPUT);
  pinMode(SPI_SCK, OUTPUT);
  digitalWrite(LCD_CS_PIN, LOW);
  digitalWrite(LCD_DC_PIN, LOW);
  digitalWrite(SPI_MOSI, LOW);
  digitalWrite(SPI_SCK, LOW);
  delay(offMs);

  // Deassert CS before restoring power, then allow the load switch and panel
  // rail to settle. s_tft.init() owns GPIO45 through TFT_RST and performs the
  // required HIGH->LOW->HIGH hardware-reset pulse before the init commands.
  digitalWrite(LCD_CS_PIN, HIGH);
  setEnabled(true);
  delay(settleMs);

  Serial.printf("[display] cold reset: rail off %u ms, settle %u ms\n",
                (unsigned)offMs, (unsigned)settleMs);
  return init();
}

TFT_eSPI& tft() {
  if (s_canvasReady && s_canvasRedirect) return s_canvas;
  return s_tft;
}

uint16_t* prepareNativeOutput(uint16_t* pixels) { return pixels; }
TFT_eSPI& directTft() {
  return s_tft;
}

void setCanvasRedirect(bool on) {
  hal::bus::lockSpi();
  s_canvasRedirect = on;
  // Restore portrait MADCTL whenever canvas redirect comes back so the flush
  // pipeline transposes correctly. Direct-mode callers set rotation 3 themselves.
  if (on) {
    s_tft.setRotation(0);
  }
  hal::bus::unlockSpi();
}

static volatile bool s_optimizedRender = false;

void enterOptimizedRender() {
  hal::bus::lockSpi();
  if (s_optimizedRender) {
    hal::bus::unlockSpi();
    return;
  }
  s_optimizedRender = true;
  s_canvasRedirect = false;
  s_tft.setRotation(3);
  hal::bus::unlockSpi();
}

void exitOptimizedRender() {
  hal::bus::lockSpi();
  if (!s_optimizedRender) {
    hal::bus::unlockSpi();
    return;
  }
  s_optimizedRender = false;
  s_canvasRedirect = true;
  s_tft.setRotation(0);
  hal::bus::unlockSpi();
  markAllDirty();
}

bool isOptimizedRender() {
  return s_optimizedRender;
}

SPIClass& spi() {
  return TFT_eSPI::getSPIinstance();
}

// ---------------------------------------------------------------------------
// Framebuffer public API
// ---------------------------------------------------------------------------

TFT_eSprite& canvas() {
  return s_canvas;
}

void markDirtyRect(int x,int y,int w,int h) { s_dirty.add(x,y,w,h); }
void markDirty(int y, int h) { markDirtyRect(0,y,kCanvasW,h); }
void markAllDirty() { markDirtyRect(0,0,kCanvasW,kCanvasH); }

static void (*s_flushService)() = nullptr;
void setFlushService(void (*service)()) { s_flushService=service; }

static void (*s_flushObserver)(uint32_t,int,int)=nullptr;
void setFlushObserver(void (*observer)(uint32_t,int,int)){s_flushObserver=observer;}

void flush() {
  if (!s_canvasReady) return;
  // No-op when canvas redirect is off — an app (e.g. MP3 now-playing) is
  // drawing direct to hardware. Pushing the stale canvas would clobber it
  // and waste SPI traffic that the app needs for SD reads.
  if (!s_canvasRedirect) return;
  if (s_dirty.bits == 0) return;

  uint32_t t0 = micros();
  if (s_flushService) s_flushService();

  const uint16_t* src = (const uint16_t*)s_canvas.getPointer();
  if (!src) return;

  // Pack each dirty rectangle into the existing rotation scratch. The windows
  // occupy disjoint row bands and therefore never exceed one full frame.
  DirtyBands::Window windows[DirtyBands::Count];
  int count=0, cursor=0, cyMin=kCanvasH, cyMax=0;
  uint32_t pixelCount=0;
  while (s_dirty.next(cursor, windows[count])) {
    const auto& r=windows[count++];
    rotateRect(src,s_rotBuf+pixelCount,kCanvasW,r.x,r.x+r.w,r.y,r.y+r.h);
    pixelCount+=r.w*r.h;
    if(r.y<cyMin)cyMin=r.y;
    if(r.y+r.h>cyMax)cyMax=r.y+r.h;
    if(count==DirtyBands::Count)break;
  }
  if (s_flushService) s_flushService();

  // Live microphone capture has a 32 ms DMA ring: retain its drain between
  // the TE wait and transfer, outside SPI ownership. Ordinary paints acquire
  // the bus first so SD contention cannot consume their synchronization edge.
  if (s_flushService) {
    waitForVSync();
    s_flushService();
  }
  hal::bus::lockSpi();
  // Mode may have changed on Core 0 after the early redirect check but before
  // this task acquired SPI. Never push a stale canvas over a direct-rendering
  // app (Music/Camera), and leave the dirty bits for the eventual mode exit.
  if (!s_canvasRedirect) {
    hal::bus::unlockSpi();
    return;
  }
  if (!s_flushService) waitForVSync();
  // INVON is an SPI command too. It must be in the same critical section as
  // the frame push; issuing it before lockSpi can land in the middle of an SD
  // sector read on the shared bus and poison an open MP3 file.
  s_tft.writecommand(0x21);
  s_tft.setSwapBytes(true);
  s_tft.startWrite();
  uint32_t offset=0;
  for(int i=0;i<count;++i) {
    const auto& r=windows[i];
    s_tft.pushImage(r.y,kCanvasW-r.x-r.w,r.h,r.w,s_rotBuf+offset);
    offset+=r.w*r.h;
  }
  s_tft.endWrite();
  s_tft.setSwapBytes(false);
  hal::bus::unlockSpi();
  const uint32_t completedMs=millis();

  s_dirty.clear();
  if (s_flushService) s_flushService();

  uint32_t elapsed = micros() - t0;
  s_lastFlushUs = elapsed;
  ++s_flushStats.frames;
  s_flushStats.pixels += pixelCount;
  s_flushStats.elapsedUs += elapsed;

  if (s_lastFlushTimeUs != 0) {
    uint32_t frameDelta = t0 - s_lastFlushTimeUs;
    if (frameDelta > 0) {
      float instantFps = 1000000.0f / (float)frameDelta;
      s_fps = s_fps * 0.9f + instantFps * 0.1f;
    }
  }
  s_lastFlushTimeUs = t0;
  if(s_flushObserver)s_flushObserver(completedMs,cyMin,cyMax);
}

FlushStats flushStats() { return s_flushStats; }
uint32_t lastFlushUs() { return s_lastFlushUs; }
float currentFps() { return s_fps; }
uint32_t teEdgeCount() { return s_teEdgeCount; }

void fixColorInversion() {
  hal::bus::lockSpi();
  s_tft.writecommand(0x21);  // ST7789 INVON
  hal::bus::unlockSpi();
}

bool waitForVSync() {
  s_vsyncFlag = false;
  const uint32_t t0 = micros();
  while (!s_vsyncFlag) {
    // FRCTR2 targets roughly 53 Hz (18.9 ms). The former 18 ms timeout could
    // expire immediately before a valid TE edge, defeating synchronization.
    if (micros() - t0 > 22000) return false;
  }
  s_vsyncFlag = false;
  return true;
}

void fillScreen(uint16_t color) {
  tft().fillScreen(color);
}

void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  tft().fillRect(x, y, w, h, color);
}

void clearContentArea(uint16_t bg) {
  setSceneChromeBackground(bg);
  TFT_eSPI& d = tft();
  const int top = kSystemStatusBarH;
  const int h   = d.height() - top;
  if (h > 0) {
    d.fillRect(0, top, d.width(), h, bg);
    markDirty(top, h);
  }
}

void setTextColor(uint16_t fg, uint16_t bg) {
  tft().setTextColor(fg, bg);
}

void setTextSize(uint8_t size) {
  tft().setTextSize(size);
}

void setCursor(int16_t x, int16_t y) {
  tft().setCursor(x, y);
}

void print(const char* s) {
  tft().print(s);
}

void print(unsigned long n) {
  tft().print(n);
}

void printf(const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  tft().print(buf);
}

bool drawJpegFrame(const uint8_t* buf, size_t len, int screenW, int screenH) {
  if (!buf || len == 0) return false;
  if (!JpegDec.decodeArray(const_cast<uint8_t*>(buf), len)) return false;

  uint16_t mcu_w = JpegDec.MCUWidth;
  uint16_t mcu_h = JpegDec.MCUHeight;

  hal::bus::lockSpi();
  s_tft.startWrite();
  while (JpegDec.readSwappedBytes()) {
    uint16_t* pImg = (uint16_t*)JpegDec.pImage;
    uint32_t dx = JpegDec.MCUx * mcu_w;
    uint32_t dy = JpegDec.MCUy * mcu_h;
    if (dx < (uint32_t)screenW && dy < (uint32_t)screenH) {
      s_tft.pushImage(dx, dy, mcu_w, mcu_h, pImg);
    }
  }
  s_tft.endWrite();
  hal::bus::unlockSpi();
  return true;
}

bool drawJpegFileScaled(const char* path,
                        int32_t dstX, int32_t dstY,
                        int32_t maxW, int32_t maxH, ImageFit fit) {
  if (!path || maxW <= 0 || maxH <= 0) return false;

  hal::bus::lockSpi();
  if (!JpegDec.decodeSdFile(path)) {
    hal::bus::unlockSpi();
    return false;
  }

  const int imgW  = (int)JpegDec.width;
  const int imgH  = (int)JpegDec.height;
  const int mcu_w = (int)JpegDec.MCUWidth;
  const int mcu_h = (int)JpegDec.MCUHeight;

  if(fit==ImageFit::Cover){
    if(imgW<=0||imgH<=0||maxW>280||maxH>240){JpegDec.abort();hal::bus::unlockSpi();return false;}
    auto* fb=static_cast<uint16_t*>(heap_caps_malloc(size_t(maxW)*maxH*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!fb){JpegDec.abort();hal::bus::unlockSpi();return false;}
    core::images::Cover cover(imgW,imgH,maxW,maxH);
    while(JpegDec.readSwappedBytes()){
      const int sx0=JpegDec.MCUx*mcu_w,sy0=JpegDec.MCUy*mcu_h;
      const int x0=cover.boundary(sx0,true),x1=cover.boundary(sx0+mcu_w,true);
      const int y0=cover.boundary(sy0,false),y1=cover.boundary(sy0+mcu_h,false);
      const auto* pixels=static_cast<const uint16_t*>(JpegDec.pImage);
      for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)
        fb[y*maxW+x]=pixels[(cover.y(y)-sy0)*mcu_w+cover.x(x)-sx0];
    }
    hal::bus::unlockSpi();
    tft().pushImage(dstX,dstY,maxW,maxH,fb);heap_caps_free(fb);return true;
  }

  float scaleW = (float)maxW / (float)imgW;
  float scaleH = (float)maxH / (float)imgH;
  float scale  = (scaleW < scaleH) ? scaleW : scaleH;
  if (scale > 1.0f) scale = 1.0f;

  const int dstW = (int)((float)imgW * scale);
  const int dstH = (int)((float)imgH * scale);
  const int offX = (int)(dstX + (maxW - dstW) / 2);
  const int offY = (int)(dstY + (maxH - dstH) / 2);

  uint16_t* fb = (uint16_t*)heap_caps_malloc(
      (size_t)dstW * (size_t)dstH * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  TFT_eSPI& c = tft();

  if (fb) {
    const float invScale = 1.0f / scale;
    while (JpegDec.readSwappedBytes()) {
      const uint16_t* pImg = (const uint16_t*)JpegDec.pImage;
      const int sx0 = (int)JpegDec.MCUx * mcu_w;
      const int sy0 = (int)JpegDec.MCUy * mcu_h;
      int dp0x = (int)((float)sx0           * scale);
      int dp0y = (int)((float)sy0           * scale);
      int dp1x = (int)((float)(sx0 + mcu_w) * scale);
      int dp1y = (int)((float)(sy0 + mcu_h) * scale);
      if (dp1x > dstW) dp1x = dstW;
      if (dp1y > dstH) dp1y = dstH;
      for (int py = dp0y; py < dp1y; py++) {
        int sy = (int)((float)py * invScale) - sy0;
        if (sy < 0) sy = 0; if (sy >= mcu_h) sy = mcu_h - 1;
        const uint16_t* srcRow = pImg + sy * mcu_w;
        uint16_t*       dstRow = fb   + py * dstW;
        for (int px = dp0x; px < dp1x; px++) {
          int sx = (int)((float)px * invScale) - sx0;
          if (sx < 0) sx = 0; if (sx >= mcu_w) sx = mcu_w - 1;
          dstRow[px] = srcRow[sx];
        }
      }
    }
    hal::bus::unlockSpi();
    c.pushImage(offX, offY, dstW, dstH, fb);
    heap_caps_free(fb);
  } else {
    // Fallback: center-crop at native resolution into canvas
    const int cropOffX = (int)(dstX + (maxW - imgW) / 2);
    const int cropOffY = (int)(dstY + (maxH - imgH) / 2);
    while (JpegDec.readSwappedBytes()) {
      const int dx = (int)(JpegDec.MCUx * mcu_w) + cropOffX;
      const int dy = (int)(JpegDec.MCUy * mcu_h) + cropOffY;
      c.pushImage(dx, dy, mcu_w, mcu_h, (uint16_t*)JpegDec.pImage);
    }
    hal::bus::unlockSpi();
  }
  return true;
}

void drawImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* rgb565Buffer) {
  if (!rgb565Buffer || w <= 0 || h <= 0) return;
  TFT_eSPI& d = tft();
  hal::bus::lockSpi();
  d.setSwapBytes(true);
  d.pushImage(x, y, w, h, const_cast<uint16_t*>(rgb565Buffer));
  d.setSwapBytes(false);
  hal::bus::unlockSpi();
}

void beginDirectDraw() { hal::bus::lockSpi(); }
void endDirectDraw()   { hal::bus::unlockSpi(); }

void setEnabled(bool on) {
  ensureControlPins();
#if LCD_ENABLE_PIN >= 0
  digitalWrite(LCD_ENABLE_PIN, on ? HIGH : LOW);
#else
  // rev4: display enable is on the expander (DISP.EN).
  hal::io::setDisplayEnable(on);
#endif
}

void setBacklight(bool on) {
  ensureControlPins();
#if LCD_BACKLIGHT_PIN >= 0
  digitalWrite(LCD_BACKLIGHT_PIN, on ? HIGH : LOW);
#else
  // rev4: backlight is on the expander (DISP.BL) — simple on/off, no PWM.
  hal::io::setBacklight(on);
#endif
}

void rampBacklight(uint16_t durationMs) {
  ensureControlPins();
#if LCD_BACKLIGHT_PIN >= 0
  // LEDC channel 2 / timer 1 — deliberately NOT channel 0 / timer 0, which the
  // esp32-camera driver uses for XCLK (hal_camera). 5 kHz @ 8-bit is flicker-free.
  constexpr uint8_t  kCh    = 2;
  constexpr uint8_t  kRes   = 8;     // 0..255 duty
  constexpr uint32_t kFreq  = 5000;
  constexpr int      kSteps = 32;
  ledcSetup(kCh, kFreq, kRes);
  ledcAttachPin(LCD_BACKLIGHT_PIN, kCh);
  for (int i = 1; i <= kSteps; i++) {
    ledcWrite(kCh, (255 * i) / kSteps);
    delay(durationMs / kSteps);
  }
  ledcWrite(kCh, 255);
  // Revert to plain GPIO so setBacklight()/sleep stay digital-consistent.
  ledcDetachPin(LCD_BACKLIGHT_PIN);
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(LCD_BACKLIGHT_PIN, HIGH);
#else
  // TODO(rev4): no PWM dimming on the expander backlight (DISP.BL is a plain
  // on/off line). Soft-start is not possible; just switch it on.
  (void)durationMs;
  hal::io::setBacklight(true);
#endif
}

void setChipSelect(bool selected) {
  ensureControlPins();
  digitalWrite(LCD_CS_PIN, selected ? LOW : HIGH);
}

bool chipSelectLevel() {
  ensureControlPins();
  return digitalRead(LCD_CS_PIN) == HIGH;
}

void releaseSpiForSd() {
  ensureControlPins();
  digitalWrite(LCD_CS_PIN, HIGH);
  digitalWrite(SD_CS, HIGH);
}

void recoverAfterSharedSpiFailure() {
  hal::bus::lockSpi();
  ensureControlPins();
  digitalWrite(LCD_CS_PIN, HIGH);
  digitalWrite(SD_CS, HIGH);

  // A failed third-party driver probe must not define the bus state seen by
  // the next frame. Reapply the panel orientation through TFT_eSPI so it opens
  // a fresh display transaction with the correct mode and clock, then issue a
  // harmless controller command in the same owned window.
  s_tft.setRotation(s_canvasRedirect ? 0 : 3);
  s_tft.startWrite();
  s_tft.writecommand(0x21);  // ST7789 INVON
  s_tft.endWrite();
  markAllDirty();
  hal::bus::unlockSpi();
  Serial.println("[display] shared SPI restored after SD failure");
}

void freeze(bool on) {
  s_frozen = on;
}

bool frozen() {
  return s_frozen;
}

}  // namespace display
}  // namespace hal
