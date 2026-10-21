#pragma once

#include <stdint.h>

class TFT_eSPI;

namespace ui {

/** Always-on status strip: clock + WiFi strength + battery. Call service() from loop, paint() after other UI. */
class SystemStatusBar {
 public:
  void service(uint32_t millis_now);
  // Returns true only when pixels changed.
  bool paint(TFT_eSPI& tft);
  // Compose a complete strip off-panel, then transfer it once. NavHandler is
  // the caller; direct-display apps never paint or erase system glyphs.
  void paintDirect();
  /** Redraw only the clock/wifi/battery glyphs (opaque) on top of whatever is
   *  already there — used to composite the bar above the recording edge glow so
   *  the ring can colour the top edge while the content stays readable. Uses the
   *  values cached by the last paint(); call paint() first each frame. */
  void paintContentOverlay(TFT_eSPI& tft);
  /** Recompute and draw only status glyphs over an already-painted bar background. */
  void paintGlyphOverlay(TFT_eSPI& tft, uint16_t bg);
  /** Next paint() runs immediately (e.g. after full-screen menu redraw). */
  void invalidate();
  void setBackgroundColor(uint16_t color);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  struct PaintStats { uint32_t paints=0, full=0, clock=0, wifi=0, battery=0; };
  PaintStats paintStats() const { return paint_stats_; }
  const char* displayedClock() const { return shown_clock_; }
  // Last painted mask, for timing-sensitive bench checks without full USB frames.
  int8_t displayedWifiTier() const { return shown_wifi_tier_; }
#endif

 private:
  /** invalidate() forces next paint regardless of cached vs live comparison. */
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  PaintStats paint_stats_{};
#endif
  uint8_t painted_parts_ = 0;
  bool last_battery_dim_ = false;
  uint8_t last_bolt_alpha_ = 255;
  bool force_repaint_ = true;
  TFT_eSPI* last_target_ = nullptr;  // Cached values describe only this drawing target.
  char last_clock_[8]{};
  int8_t last_wifi_tier_ = -1;
  uint8_t last_bat_segs_ = 0xFF;
  uint8_t last_bat_percent_ = 0xFF;
  bool last_bat_estimated_ = false;
  bool last_ext_power_ = false;
  bool last_ble_conn_ = false;
  uint16_t background_color_ = 0xFFFF;
  uint16_t last_background_color_ = 0xFFFF;

  // Snapshot of the last painted values, so paintContentOverlay() can redraw the
  // glyphs on top of the edge glow without recomputing battery/wifi/clock state.
  uint8_t  shown_segs_      = 0;
  uint8_t  shown_percent_   = 0xFF;
  bool     shown_estimated_ = false;
  bool     shown_ext_       = false;
  bool     shown_charging_ = false;
  int8_t   shown_wifi_tier_ = 0;
  bool     shown_wifi_conn_ = false;
  bool     shown_ble_conn_  = false;
  bool     shown_red_alert_ = false;
  char     shown_clock_[8]{};
};

}  // namespace ui
