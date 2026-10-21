#include "hal/hal_power.h"

#include <Arduino.h>
#include <Wire.h>

#include "Pins.h"
#include "hal/hal_bus.h"
#include "hal/hal_io.h"
#include "hal/fuel_gauge_workaround.h"
#include "core/battery_estimator.h"

namespace hal {
namespace power {

// -----------------------------------------------------------------------------
// I2C fuel gauge — rev4: BQ27220 @ 0x55 on the CORE bus (Wire, GPIO15/16).
// (rev0.2 used a MAX17048 @ 0x36 on the core bus.)
// -----------------------------------------------------------------------------
namespace {
constexpr uint8_t kBq27220Addr = 0x55;
// TI SLUUBD4A section 5.3 warns that excessive standard-command polling can
// starve the gauge watchdog. All UI/heartbeat/diagnostic callers share 1 Hz.
constexpr uint32_t kGaugeSamplePeriodMs = 1000;
constexpr uint32_t kGaugeMaxAgeMs = 5000;
FuelGaugeTelemetry g_gauge = {};
uint32_t g_lastGaugeAttemptMs = 0;
bool g_gaugeAttempted = false;
bool g_gaugeValid = false;

enum class FuelGaugeState : uint8_t { Untested, Absent, Present };
FuelGaugeState g_fgState = FuelGaugeState::Untested;
bool           g_i2cScanLogged = false;

static void logI2cScanOnce() {
  if (g_i2cScanLogged) return;
  g_i2cScanLogged = true;
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(250))) return;

  char buf[256];
  size_t n = 0;
  n += snprintf(buf + n, sizeof(buf) - n, "[i2c] core:");
  int found = 0;

  for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    const uint8_t err = (uint8_t)Wire.endTransmission(true);
    if (err == 0) {
      found++;
      n += snprintf(buf + n, sizeof(buf) - n, " 0x%02X", addr);
      if (n >= sizeof(buf) - 8) break;
    }
  }

  n += snprintf(buf + n, sizeof(buf) - n, " accessory:");
  for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
    Wire1.beginTransmission(addr);
    const uint8_t err = (uint8_t)Wire1.endTransmission(true);
    if (err == 0) {
      found++;
      n += snprintf(buf + n, sizeof(buf) - n, " 0x%02X", addr);
      if (n >= sizeof(buf) - 8) break;
    }
  }

  hal::bus::unlockI2c();
  if (found == 0) {
    Serial.println("[i2c] core/accessory: (none found)");
  } else {
    Serial.println(buf);
  }
}

static bool sampleFuelGauge(FuelGaugeTelemetry& sample) {
  // Each standard command is independent (the gauge updates live between
  // reads). Release between words so a ten-command telemetry sample cannot
  // hold the motor's shared bus across a human-visible haptic gap.
  auto readWord = [](uint8_t command, uint16_t* out) -> bool {
    if (!hal::bus::lockI2c(pdMS_TO_TICKS(100))) return false;
    // Also satisfy the >=66 us bus-free interval if core I2C runs at 400 kHz.
    delayMicroseconds(70);
    Wire.beginTransmission(kBq27220Addr);
    Wire.write(command);
    if (Wire.endTransmission(false) != 0 ||
        Wire.requestFrom((int)kBq27220Addr, 2, (int)true) != 2) {
      hal::bus::unlockI2c();
      return false;
    }
    const uint8_t lo = (uint8_t)Wire.read();
    const uint8_t hi = (uint8_t)Wire.read();
    *out = (uint16_t)lo | ((uint16_t)hi << 8);
    hal::bus::unlockI2c();
    return true;
  };

  uint16_t currentRaw = 0;
  const bool ok = readWord(0x06, &sample.temperature_deci_kelvin) &&
                  readWord(0x08, &sample.voltage_mv) &&
                  readWord(0x0A, &sample.battery_status) &&
                  readWord(0x0C, &currentRaw) &&
                  readWord(0x10, &sample.remaining_mah) &&
                  readWord(0x12, &sample.full_mah) &&
                  readWord(0x2C, &sample.percent) &&
                  readWord(0x2E, &sample.health_percent) &&
                  readWord(0x3A, &sample.operation_status) &&
                  readWord(0x3C, &sample.design_mah);
  if (!ok || sample.voltage_mv < 2000 || sample.voltage_mv > 6000)
    return false;
  sample.current_ma = (int16_t)currentRaw;
  return true;
}
}  // namespace

bool readFuelGauge(FuelGaugeTelemetry& telemetry) {
  static uint32_t lastLogMs = 0;
  static uint16_t lastLoggedPercent = 0xFFFF;
  const uint32_t now = millis();
  if (!g_gaugeAttempted || (uint32_t)(now - g_lastGaugeAttemptMs) >= kGaugeSamplePeriodMs) {
    g_gaugeAttempted = true;
    g_lastGaugeAttemptMs = now;
    FuelGaugeTelemetry sample = {};
    if (sampleFuelGauge(sample)) {
      sample.sampled_ms = millis();
      sample.sample_count = g_gauge.sample_count + 1;
      g_gauge = sample;
      g_gaugeValid = true;
      updateFuelWorkaround(sample);
      // Keep unplug/replug and discharge investigations useful without a host
      // sending commands (which can be interpreted as text by the Notes app).
      if (sample.percent != lastLoggedPercent || (uint32_t)(sample.sampled_ms - lastLogMs) >= 30000u) {
        lastLoggedPercent = sample.percent;
        lastLogMs = sample.sampled_ms;
        Serial.printf("[battery] sample=%lu SOC=%u%% voltage=%u mV current=%d mA "
                      "remaining=%u/%u mAh status=%04X op=%04X\n",
                      (unsigned long)sample.sample_count, (unsigned)sample.percent,
                      (unsigned)sample.voltage_mv, (int)sample.current_ma,
                      (unsigned)sample.remaining_mah, (unsigned)sample.full_mah,
                      (unsigned)sample.battery_status, (unsigned)sample.operation_status);
      }
    }
  }
  // Keep the last good sample over a short bus glitch, but never freeze a stale
  // percentage indefinitely if the gauge goes away. Unsigned math handles wrap.
  if (!g_gaugeValid || (uint32_t)(millis() - g_gauge.sampled_ms) >= kGaugeMaxAgeMs)
    return false;
  telemetry = g_gauge;
  return true;
}

// -----------------------------------------------------------------------------
// VBAT ADC sampling
// -----------------------------------------------------------------------------
//
// We treat the GPIO defined by BAT_ADC_PIN as a divided VBAT sense. The ADC
// is sampled at ~kSamplePeriodMs cadence, averaged over kAvgSamples reads to
// reject spikes, then EMA-smoothed so a single dip during a Wi-Fi TX burst
// does not knock a bar off the icon. UI code calls `read()` which returns the
// smoothed value — `readBatteryMillivoltsRaw()` is exposed for diagnostics.
//
// `analogReadMilliVolts()` already applies the per-chip ADC calibration curve
// stored in eFuse, so the only post-processing here is multiplying by the
// divider ratio (BAT_ADC_DIVIDER, e.g. 2.0 for the typical 100k/100k tap).

namespace {
constexpr uint32_t kSamplePeriodMs = 500;   // ADC sample cadence
constexpr int      kAvgSamples     = 8;     // average per sample to denoise
constexpr float    kEmaAlpha       = 0.2f;  // smoothing weight (0=frozen, 1=instant)
// Reject obviously wrong readings. On boards where `BAT_ADC_PIN` is accidentally tied to a pulled-up
// digital net (e.g. an IRQ line), the computed "VBAT" can exceed 5 V; we treat that as no divider.
constexpr uint16_t kMaxPlausibleBatteryMv = 4600;

bool     g_adcConfigured = false;
uint32_t g_lastSampleMs  = 0;
float    g_smoothedMv    = 0.0f;  // 0 until first valid sample
bool     g_primedOnce    = false;

void ensureAdcConfigured() {
  if (g_adcConfigured) return;
#if BAT_ADC_PIN >= 0
  pinMode(BAT_ADC_PIN, INPUT);
  // Set ADC attenuation wide enough to cover the divider midpoint.
#if defined(ARDUINO_ARCH_ESP32)
  analogSetPinAttenuation((uint8_t)BAT_ADC_PIN, (adc_attenuation_t)BAT_ADC_ATTENUATION_DB);
#endif
  g_adcConfigured = true;
#endif
}

uint16_t sampleAveragedMv() {
#if BAT_ADC_PIN >= 0
  ensureAdcConfigured();
  uint32_t acc = 0;
  for (int i = 0; i < kAvgSamples; i++) {
    acc += analogReadMilliVolts((uint8_t)BAT_ADC_PIN);
  }
  const uint32_t avg = acc / kAvgSamples;
  const uint16_t mv = (uint16_t)((float)avg * BAT_ADC_DIVIDER);
  if (mv > kMaxPlausibleBatteryMv) return 0;
  return mv;
#else
  return 0;
#endif
}

void updateSmoothedIfDue(uint32_t now) {
#if BAT_ADC_PIN >= 0
  if (!g_primedOnce) {
    // Prime the EMA once. Even if the sample is 0 (pin not wired), don't spam logs forever.
    g_primedOnce    = true;
    g_smoothedMv    = (float)sampleAveragedMv();
    g_lastSampleMs  = now;
    Serial.printf("[battery] first sample on GPIO %d: raw=%u mV (divider=%.2f)\n",
                  (int)BAT_ADC_PIN, (unsigned)g_smoothedMv, (double)BAT_ADC_DIVIDER);
    return;
  }
  if ((uint32_t)(now - g_lastSampleMs) < kSamplePeriodMs) return;
  g_lastSampleMs    = now;
  const float fresh = (float)sampleAveragedMv();
  g_smoothedMv      = (1.0f - kEmaAlpha) * g_smoothedMv + kEmaAlpha * fresh;
  // Throttled diagnostic log so you can sanity-check the divider ratio: a
  // healthy LiPo should land between ~3.3 V and ~4.2 V. If you see ~1.6 V the
  // divider is 2:1 and the firmware needs `BAT_ADC_DIVIDER 2.0f`; if you see
  // ~3.3 V at 100% the pin is reading raw VBAT through a higher divider
  // (adjust the constant). If you see 0 or 3300 mV pinned, the pin is wrong.
  static uint32_t s_lastDbg = 0;
  if ((uint32_t)(now - s_lastDbg) >= 5000u) {
    s_lastDbg = now;
    Serial.printf("[battery] GPIO %d: smoothed=%u mV  fresh=%u mV  pct=%u\n",
                  (int)BAT_ADC_PIN, (unsigned)g_smoothedMv, (unsigned)fresh,
                  (unsigned)batteryPercentFromMv((uint16_t)g_smoothedMv));
  }
#else
  (void)now;
#endif
}
}  // namespace

uint16_t readBatteryMillivoltsRaw() { return sampleAveragedMv(); }

// -----------------------------------------------------------------------------
// Voltage -> state-of-charge mapping
// -----------------------------------------------------------------------------
//
uint8_t batteryPercentFromMv(uint16_t mv) {
  return core::battery::voltagePercent(mv);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

Status read() {
  Status s = {};
  s.pg_active_low    = hal::io::chargerPgActiveLow();
  s.chg_active_low   = hal::io::chargerChgActiveLow();
  s.stats_active_low = s.pg_active_low;  // legacy alias = "USB present"

  // BQ24040 dual-line decode (PG/CHG are open-drain outputs, both active LOW):
  //   PG=H, CHG=H  → Off
  //   PG=L, CHG=L  → Charging
  //   PG=L, CHG=H  → Done
  //   PG=H, CHG=L  → Unknown
  if (s.pg_active_low && s.chg_active_low) {
    s.charger = ChargerState::Charging;
  } else if (s.pg_active_low && !s.chg_active_low) {
    s.charger = ChargerState::Done;
  } else if (!s.pg_active_low && !s.chg_active_low) {
    s.charger = ChargerState::Off;
  } else {
    s.charger = ChargerState::Unknown;
  }

  // Battery telemetry source priority:
  //   1) BQ27220 voltage/current, with optional host SOC workaround.
  //   2) ADC divider (if present) → estimated SoC% from voltage.
  FuelGaugeTelemetry gauge = {};
  if (readFuelGauge(gauge)) {
    if (g_fgState != FuelGaugeState::Present) {
      g_fgState = FuelGaugeState::Present;
      Serial.println("[battery] BQ27220 detected on core I2C (0x55)");
    }
    s.battery_mv = gauge.voltage_mv;
    const bool present = (gauge.battery_status & (1u << 3)) != 0;
    const bool initialized = (gauge.operation_status & (1u << 5)) != 0;
    const bool suspended = (gauge.operation_status & ((1u << 10) | 1u)) != 0;
    s.battery_estimated = fuelWorkaroundEnabled();
    s.battery_percent = s.battery_estimated ? fuelWorkaroundPercent() :
        (present && initialized && !suspended && gauge.percent <= 100 ? (uint8_t)gauge.percent : 0xFF);
    return s;
  }
  if (!g_i2cScanLogged) {
    Serial.println("[battery] BQ27220 telemetry unavailable; using ADC fallback");
    logI2cScanOnce();
  }

  updateSmoothedIfDue(millis());
  s.battery_mv = (uint16_t)g_smoothedMv;
  s.battery_percent =
      (s.battery_mv == 0) ? (uint8_t)0xFF : batteryPercentFromMv(s.battery_mv);
  s.battery_estimated = true;
  return s;
}

uint8_t batterySegments(const Status& s) {
  // Real measurement available — bucket the percent into 0..4 bars.
  // Bucket boundaries chosen so each bar represents a comparable amount of
  // remaining runtime (the bottom bar lights up early because the curve
  // bottom is steep, and "full" requires 80% so a freshly-disconnected
  // charger does not lie about a half-empty cell).
  if (batterySocKnown(s)) {
    const uint8_t p = s.battery_percent;
    if (p >= 80) return 4;
    if (p >= 55) return 3;
    if (p >= 30) return 2;
    if (p >= 10) return 1;
    return 0;
  }

  // Charger pins indicate input power, not remaining charge. Never manufacture
  // a full battery when telemetry is unavailable; the UI also displays "--%".
  return 0;
}

}  // namespace power
}  // namespace hal
