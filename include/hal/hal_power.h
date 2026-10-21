#pragma once

#include <stdint.h>

namespace hal {
namespace power {

/** Decoded charger state from BQ24040 dual PG / CHG status outputs.
 *
 * Truth table (active-low encoding — `true` = pin reads LOW = "asserted"):
 *   - pg=0, chg=0 → Off       (no USB / adapter input)
 *   - pg=1, chg=1 → Charging  (USB present, charger pushing current into the cell)
 *   - pg=1, chg=0 → Done      (USB present, cell at regulation voltage)
 *   - pg=0, chg=1 → Unknown   (impossible / fault / glitch)
 */
enum class ChargerState : uint8_t { Off = 0, Charging, Done, Unknown };

/** Charger PG/CHG (open drain via IO expander) plus battery telemetry. */
struct Status {
  /** PG (power-good) asserted low = "USB / adapter present". */
  bool pg_active_low;
  /** CHG asserted low = "actively charging". */
  bool chg_active_low;
  /** Legacy alias for older call sites: equals `pg_active_low`. */
  bool stats_active_low;
  /** Decoded charger state per BQ24040 truth table (independent of VBAT ADC). */
  ChargerState charger;
  /** Gauge voltage (or smoothed ADC voltage) in millivolts. 0 if unavailable. */
  uint16_t battery_mv;
  /** Gauge state-of-charge (or ADC estimate), 0..100. 0xFF if unavailable. */
  uint8_t battery_percent;
  /** True for the provisional host estimator or voltage fallback. */
  bool battery_estimated;
};

Status read();

/** Read-only BQ27220 diagnostics, shared with the UI's 1 Hz sample cache.
 * Call from ui_task, like read(); false if telemetry is absent or older than 5 s. */
struct FuelGaugeTelemetry {
  uint16_t temperature_deci_kelvin;
  uint16_t voltage_mv;
  uint16_t battery_status;
  int16_t current_ma;
  uint16_t remaining_mah;
  uint16_t full_mah;
  uint16_t percent;
  uint16_t health_percent;
  uint16_t operation_status;
  uint16_t design_mah;
  uint32_t sampled_ms;
  uint32_t sample_count;
};
bool readFuelGauge(FuelGaugeTelemetry& telemetry);

/**
 * Best-effort "on adapter/USB" for UI/idle: true when the BQ24040 reports either Charging or Done
 * (either way an input source is present). With dual-line STAT decoding we no longer have to
 * collapse "Done" into "on battery" the way the single-line code did.
 */
inline bool externalPowerPresent(const Status& s) {
  return s.charger == ChargerState::Charging || s.charger == ChargerState::Done;
}

/** True while the charger IC is actively pushing current into the cell (STAT1 asserted). */
inline bool charging(const Status& s) { return s.charger == ChargerState::Charging; }

/** True when a valid gauge or ADC state-of-charge measurement is available. */
inline bool batterySocKnown(const Status& s) { return s.battery_percent <= 100; }

/** 0–4 filled segments from measured charge; empty outline when unavailable. */
uint8_t batterySegments(const Status& s);

/**
 * Raw / immediate single-sample VBAT read in millivolts (no smoothing). Returns 0
 * if BAT_ADC_PIN is disabled. Prefer `read().battery_mv` for UI — that one is EMA-smoothed.
 */
uint16_t readBatteryMillivoltsRaw();

/**
 * Map a Li-ion cell voltage to an estimated state-of-charge percent (0..100).
 * Uses a provisional piecewise-linear curve, not a characterized cell profile.
 * Returns 0xFF if `mv == 0` (no ADC pin).
 */
uint8_t batteryPercentFromMv(uint16_t mv);

}  // namespace power
}  // namespace hal
