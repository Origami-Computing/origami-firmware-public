#include "ui/system_status_bar.h"

#include <Arduino.h>

#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "services/automation.h"
#include "core/board_time.h"
#include "hal/hal_ble.h"
#include "hal/hal_power.h"
#include "hal/hal_wifi.h"
#include "ui/text_field.h"
#include "ui/alpha_bitmap.h"
#include "assets/battery_icon.h"
#include "assets/wifi_icons.h"
#include "ui/wifi_icon.h"
#include "ui/wifi_connecting_animation.h"
#include "ui/status_bar_palette.h"
#include "hal/hal_display.h"
#include "ui/safe_area_draw.h"
#include "ui/battery_indicator.h"
#include "ui/instrument_text.h"
#include "ui/proportional_text.h"
#include "services/battery_alerts.h"
#include "services/device_fs.h"
#include "services/uploads.h"

#include <TB_TFT_eSPI.h>
#include <WiFi.h>
#include <string.h>

namespace ui {

namespace {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool s_batteryPreview=false;
int s_batteryPreviewPower=0;
uint8_t s_boltAlpha=255; bool s_shownCharging=false;
uint8_t s_batteryPreviewPercent=255, s_batteryShownPercent=255;
int s_batteryPreviewPhase=-1;
uint32_t s_batteryPreviewStarted=0, s_batteryShownMs=0;
bool s_batteryShownExternal=false;
uint16_t s_batteryShownOutline=0, s_batteryShownFill=0;
int s_batteryX=0,s_batteryY=0;
#endif
uint32_t batteryAnimationTime() {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(s_batteryPreview && s_batteryPreviewPhase>=0)
    return s_batteryPreviewPhase*ui::battery::kFlashHalfMs;
#endif
  return millis();
}
hal::power::Status batteryStatusForPaint() {
  auto p=hal::power::read();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(s_batteryPreview && uint32_t(millis()-s_batteryPreviewStarted)>=120000) { s_batteryPreview=false; services::battery_alerts::setPreview(-1,0,false); }
  if(s_batteryPreview) {
    p.battery_percent=s_batteryPreviewPercent;
    p.charger=s_batteryPreviewPower==1 ? hal::power::ChargerState::Charging : s_batteryPreviewPower==2 ? hal::power::ChargerState::Done : hal::power::ChargerState::Off;
  }
#endif
  return p;
}
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void battery::setPreview(int percent,int power,int phase,bool audio) {
  services::battery_alerts::setPreview(percent,power,audio);
  s_batteryPreview=percent>=0;
  s_batteryPreviewPercent=uint8_t(percent);
  s_batteryPreviewPower=power;
  s_batteryPreviewPhase=phase;
  s_batteryPreviewStarted=millis();
}
void battery::printPreviewStatus() {
  Serial.printf("[battery-icon] preview=%d percent=%u external=%d outline=%04x fill=%04x ms=%lu x=%d y=%d sound=enabled\n",
    s_batteryPreview,s_batteryShownPercent,s_batteryShownExternal,s_batteryShownOutline,s_batteryShownFill,(unsigned long)s_batteryShownMs,s_batteryX,s_batteryY);
  const auto cues=services::battery_alerts::diagnostics();
  Serial.printf("[battery-cues] low=%lu plug=%lu skipped=%lu frames=%lu charging=%d alpha=%u fixtureAudio=%d volume=%d\n",
    (unsigned long)cues.low,(unsigned long)cues.plug,(unsigned long)cues.skipped,
    (unsigned long)cues.frames,s_shownCharging,s_boltAlpha,cues.fixtureAudio,cues.volume);
}
#endif

/** Instant RSSI tier from driver (disconnected → 0). */
static int8_t wifiTierRawFromEnv() {
  if (!hal::wifi::connected()) return 0;
  int8_t rssi = WiFi.RSSI();
  if (rssi > 0) return 4;
  if (rssi == 0) return 3;
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  return 1;
}

// One animation clock for normal chrome and direct-to-panel overlays. No radio
// mutations here: wifi_mgr owns the join/retry lifecycle.
static int8_t wifiTierForPaint(bool connected, uint32_t now, int8_t connectedTier) {
  static bool animating = false;
  static uint32_t started = 0;
  const bool connecting = !connected && hal::wifi::connecting();
  if (connecting && !animating) started = now;
  animating = connecting;
  return connected ? connectedTier : connecting ? wifiConnectingTier(now - started) : 0;
}

static void drawBleGlyph(TFT_eSPI& tft, int x, int y, bool connected, uint16_t connInk,
                         uint16_t discInk, uint16_t bg, bool transparent = false) {
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  if (transparent) tft.setTextColor(connected ? connInk : discInk);
  else tft.setTextColor(connected ? connInk : discInk, bg);
  tft.drawString("B", x, y + 5);
}

// A cloud with one directional arrow and a blue count badge. The cloud stays
// still; sequential dots travel toward the transfer arrowhead.
static void drawSyncGlyph(TFT_eSPI& tft, int cx, int cy, uint8_t frame,
                          uint8_t direction, uint8_t count, uint16_t ink) {
  if (!count) return;
  auto boldLine = [&](int x0,int y0,int x1,int y1,uint16_t color) {
    for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx)
      tft.drawLine(x0+dx,y0+dy,x1+dx,y1+dy,color);
  };
  const bool blocked = direction == 3;
  const uint16_t badgeColor = blocked ? 0xF800 : 0x1B5F;
  // Wi-Fi-width cloud with an open base for the transfer arrow.
  boldLine(cx-3,cy+3,cx-8,cy+3,ink);
  boldLine(cx-8,cy+3,cx-10,cy+1,ink);
  boldLine(cx-10,cy+1,cx-10,cy-3,ink);
  boldLine(cx-10,cy-3,cx-8,cy-5,ink);
  boldLine(cx-8,cy-5,cx-5,cy-6,ink);
  boldLine(cx-5,cy-6,cx-3,cy-9,ink);
  boldLine(cx-3,cy-9,cx+0,cy-10,ink);
  boldLine(cx+0,cy-10,cx+3,cy-9,ink);
  boldLine(cx+3,cy-9,cx+5,cy-6,ink);
  boldLine(cx+5,cy-6,cx+7,cy-5,ink);
  boldLine(cx+7,cy-5,cx+10,cy-2,ink);
  boldLine(cx+10,cy-2,cx+10,cy+0,ink);
  if (blocked) {
    boldLine(cx-3,cy+3,cx+10,cy+3,ink);
    boldLine(cx+10,cy+0,cx+10,cy+3,ink);
  }
  // Dots, then shaft, then arrowhead; hold the completed arrow.
  const int phase = frame % 16;
  const bool fullArrow = phase >= 6;
  const int dots = phase / 2 + 1;
  const int ax = cx;
  if (direction == 1 || direction == 2) {
    if (fullArrow) tft.fillRect(ax, cy-5, 2, direction == 2 ? 15 : 12, ink);
    else for (int i = 0; i < dots; ++i) {
      const int y = direction == 2 ? cy - 5 + i * 4 : cy + 5 - i * 4;
      tft.fillRect(ax, y, 2, 2, ink);
    }
    const int tip = direction == 2 ? cy + 9 : cy - 5;
    const int wing = direction == 2 ? tip - 3 : tip + 3;
    if (phase >= 8) {
      boldLine(ax-3,wing,ax,tip,ink);
      boldLine(ax,tip,ax+3,wing,ink);
    }
  }
  const int radius = count > 99 ? 9 : count > 9 ? 8 : 7;
  tft.fillCircle(cx+13,cy+5,radius,badgeColor);
  char label[4];
  if (count > 99) strcpy(label, "99+");
  else snprintf(label, sizeof(label), "%u", static_cast<unsigned>(count));
  tft.setTextFont(1); tft.setTextSize(1);
  tft.setTextColor(0xFFFF);
  tft.setTextDatum(MC_DATUM);
  ui::proportional::centered(tft,label,cy+5-ui::proportional::height(label,12)/2,
                             12,0xFFFF,badgeColor,cx+13);
}


void SystemStatusBar::service(uint32_t millis_now) {
  core::time::service(millis_now);

}

void SystemStatusBar::invalidate() {
  force_repaint_ = true;
}

void SystemStatusBar::setBackgroundColor(uint16_t color) {
  if (background_color_ == color) return;
  background_color_ = color;
  force_repaint_ = true;
}

static ui::TextField s_clockField;
enum BarPart : uint8_t { WifiPart = 1, BatteryPart = 2, ClockPart = 4, AllParts = 7 };

static void paintClockCentered(TFT_eSPI& tft, int w, int barH, const char* clockBuf, uint16_t fgInk,
                               uint16_t barBg) {
  const auto safe = safe_area::contentBoundary();
  s_clockField.style(1, 2, MC_DATUM, fgInk, barBg).at(safe.x+safe.w/2, (safe.y+barH)/2);
  // An unsynchronized clock is unavailable, not invisible. Keep the same
  // full-contrast, fixed-width field without inventing a time or arming alarms.
  s_clockField.draw(tft, clockBuf && clockBuf[0] ? clockBuf : "--:--");
}

// Draw the wifi/ble/battery/clock glyphs (each with an opaque `barBg` so it stays
// readable over any background). Shared by paint() and paintContentOverlay().
static void drawBarGlyphs(TFT_eSPI& tft, int w, int barH, uint8_t segs, uint8_t percent, bool estimated, bool ext, bool charging,
                          const char* clockBuf, int8_t wifiTierPaint, bool wifiConn,
                          bool bleConn, bool redAlertBar, uint8_t syncCount,
                          uint8_t syncDirection, uint8_t syncFrame, uint16_t barBg,
                          bool transparentClock = false, uint8_t parts = AllParts) {
  const uint16_t ink=statusBarInk(barBg);
  const auto safe = safe_area::contentBoundary();
  const int wifiTop=max(safe.y,(safe.y+barH-kWifiIconSize)/2);
  const auto wifi = safe_area::fitBox({kScreenInsetX,wifiTop,kWifiIconSize,kWifiIconSize},safe_area::Region::Screen);
  if (parts & WifiPart) {
    drawWifiIcon(tft,wifi.x,wifi.y,wifiTierPaint,wifiConn || wifiTierPaint > 0,ink,barBg,transparentClock);
    // Bluetooth is supplementary and appears only when connected.
    if(bleConn) drawBleGlyph(tft,wifi.x+33,(safe.y+barH-20)/2,true,ink,ink,barBg,transparentClock);
    drawSyncGlyph(tft, 64, (safe.y+barH)/2, syncFrame, syncDirection, syncCount, ink);
  }
  if (parts & BatteryPart) {
    const int bw=assets::battery_icon::kWidth,bh=assets::battery_icon::kHeight;
    const auto battery = safe_area::fitBox({w-kScreenInsetX-bw,(safe.y+barH-bh)/2,bw,bh},safe_area::Region::Screen);
    const int bx=battery.x,by=battery.y;
    const bool critical = ui::battery::critical(percent) && !ext;
    const bool black = ui::battery::blackPhase(percent,ext,batteryAnimationTime());
    const uint16_t outline = critical ? (black ? 0x0000 : ui::battery::kRed) : ink;
    const uint16_t fill = critical ? outline : ui::battery::color(percent);
    // Preserve the existing alpha-mask geometry and proportional fill in both phases.
    drawAlphaBitmap(tft,bx,by,bw,bh,assets::battery_icon::kAlpha4,bw,bh,outline,barBg,false);
    tft.fillRect(bx+ui::battery::kFillX,by+ui::battery::kFillY,
                 ui::battery::kFillW,ui::battery::kFillH,0xFFFF);
    tft.fillRect(bx+ui::battery::kFillX,by+ui::battery::kFillY,
                 ui::battery::fillWidth(percent),ui::battery::kFillH,fill);
    // Reserve/clear the bolt area even when unplugged, including overlay paints.
    tft.fillRect(bx-12,by+7,9,11,barBg);
    const uint16_t boltInk=ui::battery::blend(ink,barBg,ui::battery::boltAlpha(ext,batteryAnimationTime()));
    if (ext) {
      static const uint8_t bolt[] = {0x0C,0x1C,0x18,0x30,0x7E,0x0C,0x18,0x10,0x20};
      for (int y=0;y<9;++y) for (int x=0;x<7;++x)
        if (bolt[y] & (1 << (6-x))) tft.drawPixel(bx-12+x,by+8+y,boltInk);
    }
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    s_boltAlpha=ui::battery::boltAlpha(ext,batteryAnimationTime()); s_shownCharging=charging;
    s_batteryShownPercent=percent; s_batteryShownExternal=ext;
    s_batteryShownOutline=outline; s_batteryShownFill=fill;
    s_batteryShownMs=millis(); s_batteryX=bx; s_batteryY=by;
#endif
    char value[5];
    if(percent<=100) snprintf(value,sizeof(value),"%u%%",(unsigned)percent);
    else strcpy(value,"--%");
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextWrap(false,false);
    tft.setTextDatum(MC_DATUM);
    // Transparent text keeps the proportional fill visible behind the number.
    tft.setTextColor(critical ? outline : 0x0000);
    #ifdef ORIGAMI_INSTRUMENT_FONT
      const auto& face=instrument::battery;
      ui::instrument_text::text(tft,value,
          bx+assets::battery_icon::kTextCenterX-ui::instrument_text::width(value,face)/2,
          by+(bh-face.height)/2,face,critical?outline:0x0000);
#else
      tft.drawString(value,bx+assets::battery_icon::kTextCenterX,by+bh/2);
#endif
  }
  if (parts & ClockPart) {
    s_clockField.invalidate();
    if (transparentClock) {
      tft.setTextSize(2);
      tft.setTextColor(ink);
      tft.drawString(clockBuf && clockBuf[0] ? clockBuf : "--:--",safe.x+safe.w/2,(safe.y+barH)/2);
    } else paintClockCentered(tft,w,barH,clockBuf,ink,barBg);
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
}

struct SyncIndicatorState { uint8_t count = 0, direction = 0, frame = 0; };

static SyncIndicatorState syncIndicatorState(bool wifiConnected, uint32_t now) {
  services::device_fs::SyncStatus fileSync;
  services::device_fs::getSyncStatus(fileSync);
  const auto* uploadQueue = services::uploads::readyQueue();
  const auto uploads = uploadQueue ? uploadQueue->counts() : UploadQueue::Counts{};
  const uint32_t total = fileSync.remaining + uploads.pending + uploads.active + uploads.failed;
  SyncIndicatorState state;
  state.count = total > 100 ? 100 : static_cast<uint8_t>(total);
  if (!state.count) return state;
  if (!wifiConnected || (!fileSync.ok && fileSync.remaining) || uploads.failed || uploads.retrying) {
    state.direction = 3; // Closed cloud, red badge, no arrow.
    return state;
  }
  state.frame = 15; // Queued work holds the complete arrow.
  const auto fileDirection = fileSync.ok && fileSync.remaining ? fileSync.direction : services::device_fs::SyncStatus::Direction::None;
  state.direction = fileDirection == services::device_fs::SyncStatus::Direction::Upload ? 1
      : fileDirection == services::device_fs::SyncStatus::Direction::Download ? 2
      : uploads.active || uploads.pending ? 1 : 0;
  if ((fileSync.active && fileDirection != services::device_fs::SyncStatus::Direction::None) || uploads.active)
    state.frame = static_cast<uint8_t>((now / 100u) % 32u);
  return state;
}

bool SystemStatusBar::paint(TFT_eSPI& tft) {

  static bool sLastWifiConn = false;
  static int8_t sWifiShownTier    = -1;
  static int8_t sWifiCandidateTier = -1;
  static uint32_t sWifiTierSinceMs = 0;

  constexpr uint32_t kWifiTierHoldMs = 520;

  hal::power::Status p = batteryStatusForPaint();
  uint8_t segs = hal::power::batterySegments(p);
  bool ext = hal::power::externalPowerPresent(p);
  char clockBuf[8];
  core::time::formatClockHHMM(clockBuf, sizeof(clockBuf));
  const bool wifiConn = hal::wifi::connected();
  const bool bleConn  = hal::ble::clientConnected();
  const auto sync = syncIndicatorState(wifiConn, millis());
  const bool redAlertBar = services::automation::activeAlertUsesRedStatusBar();

  const int8_t measuredTier = wifiTierRawFromEnv();
  const uint32_t paintMs = millis();
  const uint8_t boltAlpha=ui::battery::boltAlpha(ext,batteryAnimationTime());
  const bool batteryDim = ui::battery::blackPhase(p.battery_percent,ext,batteryAnimationTime());

  if (!wifiConn) {
    sWifiShownTier    = 0;
    sWifiCandidateTier = -1;
  } else {
    if (sWifiShownTier <= 0) sWifiShownTier = measuredTier;
    if (measuredTier == sWifiShownTier) {
      sWifiCandidateTier = measuredTier;
      sWifiTierSinceMs   = paintMs;
    } else {
      if (measuredTier != sWifiCandidateTier) {
        sWifiCandidateTier = measuredTier;
        sWifiTierSinceMs   = paintMs;
      } else if ((uint32_t)(paintMs - sWifiTierSinceMs) >= kWifiTierHoldMs) {
        sWifiShownTier = measuredTier;
      }
    }
  }

  const int8_t wifiTierPaint = wifiTierForPaint(wifiConn, paintMs, sWifiShownTier);

  const bool full = force_repaint_ || last_target_ != &tft ||
                    background_color_ != last_background_color_;
  const bool clockChanged = strcmp(clockBuf, last_clock_) != 0;
  const bool wifiChanged = wifiConn != sLastWifiConn ||
                          wifiTierPaint != last_wifi_tier_ || bleConn != last_ble_conn_ ||
                          sync.count != last_sync_count_ || sync.direction != last_sync_direction_ ||
                          sync.frame != last_sync_frame_;
  const bool batteryChanged = hal::power::charging(p) != shown_charging_ ||
      last_bolt_alpha_ != boltAlpha || batteryDim != last_battery_dim_ ||
      segs != last_bat_segs_ || p.battery_percent != last_bat_percent_ ||
      p.battery_estimated != last_bat_estimated_ || ext != last_ext_power_;
  uint8_t parts = full ? AllParts :
      (clockChanged ? ClockPart : 0) | (wifiChanged ? WifiPart : 0) |
      (batteryChanged ? BatteryPart : 0);
  if (!parts) return false;
  painted_parts_ = parts;

  const int w = tft.width();
  const int barH = kSystemStatusBarH;
  const uint16_t barBg = background_color_;
  // Three disjoint retained fields. Clear/repaint only the field whose visible
  // state changed; navigation invalidates all fields when their backing changes.
  // The existing glyph geometry fits within these thirds (including BLE/bolt).
  if (full) {
    tft.fillRect(0, 0, w, barH, barBg);
    hal::display::markDirty(0, barH);
  } else {
    if (parts & WifiPart) {
      tft.fillRect(0, 0, w/3, barH, barBg);
      hal::display::markDirtyRect(0, 0, w/3, barH);
    }
    if (parts & ClockPart) {
      tft.fillRect(w/3, 0, 2*w/3-w/3, barH, barBg);
      hal::display::markDirtyRect(w/3, 0, 2*w/3-w/3, barH);
    }
    if (parts & BatteryPart) {
      tft.fillRect(2*w/3, 0, w-2*w/3, barH, barBg);
      hal::display::markDirtyRect(2*w/3, 0, w-2*w/3, barH);
    }
  }
  drawBarGlyphs(tft, w, barH, segs, p.battery_percent, p.battery_estimated,
                ext, hal::power::charging(p), clockBuf, wifiTierPaint,
                wifiConn, bleConn, redAlertBar, sync.count, sync.direction, sync.frame,
                barBg, false, parts);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  ++paint_stats_.paints;
  if (full) ++paint_stats_.full;
  if (parts & ClockPart) ++paint_stats_.clock;
  if (parts & WifiPart) ++paint_stats_.wifi;
  if (parts & BatteryPart) ++paint_stats_.battery;
#endif

  strncpy(last_clock_, clockBuf, sizeof(last_clock_));
  last_clock_[sizeof(last_clock_) - 1] = '\0';
  last_wifi_tier_      = wifiTierPaint;
  last_bat_segs_       = segs;
  last_bat_percent_    = p.battery_percent;
  last_bat_estimated_  = p.battery_estimated;
  last_ext_power_      = ext;
  sLastWifiConn        = wifiConn;
  last_ble_conn_       = bleConn;
  last_sync_count_ = sync.count;
  last_sync_direction_ = sync.direction;
  last_sync_frame_ = sync.frame;
  last_background_color_ = background_color_;
  force_repaint_       = false;
  last_battery_dim_ = batteryDim;
  last_bolt_alpha_ = boltAlpha;
  last_target_ = &tft;

  // Snapshot for paintContentOverlay().
  shown_segs_      = segs;
  shown_percent_   = p.battery_percent;
  shown_estimated_ = p.battery_estimated;
  shown_ext_       = ext;
  shown_charging_ = hal::power::charging(p);
  shown_wifi_tier_ = wifiTierPaint;
  shown_wifi_conn_ = wifiConn;
  shown_ble_conn_  = bleConn;
  shown_red_alert_ = redAlertBar;
  shown_sync_count_ = sync.count;
  shown_sync_direction_ = sync.direction;
  shown_sync_frame_ = sync.frame;
  strncpy(shown_clock_, clockBuf, sizeof(shown_clock_));
  shown_clock_[sizeof(shown_clock_) - 1] = '\0';
  return true;
}

void SystemStatusBar::paintDirect() {
  static TFT_eSprite strip(&hal::display::directTft());
  if (!strip.created()) {
    strip.setColorDepth(16);
    if (!strip.createSprite(280,kSystemStatusBarH)) return;
  }
  if (!paint(strip)) return;
  // Never expose the background clear or individual glyph writes on the panel.
  hal::display::beginDirectDraw();
  // Cropped transfers preserve the clock in direct mode too. Each region is
  // already completely composed in the retained strip before taking SPI.
  const int w=strip.width();
  if (painted_parts_ == AllParts) strip.pushSprite(0,0);
  else {
    if (painted_parts_ & WifiPart) strip.pushSprite(0,0,0,0,w/3,kSystemStatusBarH);
    if (painted_parts_ & ClockPart) strip.pushSprite(w/3,0,w/3,0,2*w/3-w/3,kSystemStatusBarH);
    if (painted_parts_ & BatteryPart) strip.pushSprite(2*w/3,0,2*w/3,0,w-2*w/3,kSystemStatusBarH);
  }
  hal::display::endDirectDraw();
}

void SystemStatusBar::paintContentOverlay(TFT_eSPI& tft) {
  const int w = tft.width();
  const int barH = kSystemStatusBarH;
  const uint16_t barBg = background_color_;
  drawBarGlyphs(tft, w, barH, shown_segs_, shown_percent_, shown_estimated_, shown_ext_, shown_charging_, shown_clock_, shown_wifi_tier_,
                shown_wifi_conn_, shown_ble_conn_, shown_red_alert_, shown_sync_count_, shown_sync_direction_, shown_sync_frame_, barBg,
                /*transparentClock=*/true);
}

void SystemStatusBar::paintGlyphOverlay(TFT_eSPI& tft, uint16_t bg) {
  hal::power::Status p = batteryStatusForPaint();
  const uint8_t segs = hal::power::batterySegments(p);
  const bool ext = hal::power::externalPowerPresent(p);
  char clockBuf[8];
  core::time::formatClockHHMM(clockBuf, sizeof(clockBuf));
  const bool wifiConn = hal::wifi::connected();
  const bool bleConn = hal::ble::clientConnected();
  const bool redAlertBar = services::automation::activeAlertUsesRedStatusBar();
  const auto sync = syncIndicatorState(wifiConn, millis());
  const int8_t wifiTierPaint = wifiTierForPaint(wifiConn, millis(), wifiTierRawFromEnv());
  const uint16_t barBg = bg;

  drawBarGlyphs(tft, tft.width(), kSystemStatusBarH, segs, p.battery_percent, p.battery_estimated, ext, hal::power::charging(p), clockBuf, wifiTierPaint,
                wifiConn, bleConn, redAlertBar, sync.count, sync.direction, sync.frame, barBg, /*transparentClock=*/true);
  hal::display::markDirty(0, kSystemStatusBarH);

  shown_segs_ = segs;
  shown_percent_ = p.battery_percent;
  shown_estimated_ = p.battery_estimated;
  shown_ext_ = ext;
  shown_charging_ = hal::power::charging(p);
  shown_wifi_tier_ = wifiTierPaint;
  shown_wifi_conn_ = wifiConn;
  shown_ble_conn_ = bleConn;
  shown_red_alert_ = redAlertBar;
  shown_sync_count_ = sync.count;
  shown_sync_direction_ = sync.direction;
  shown_sync_frame_ = sync.frame;
  strncpy(shown_clock_, clockBuf, sizeof(shown_clock_));
  shown_clock_[sizeof(shown_clock_) - 1] = '\0';
}

}  // namespace ui

