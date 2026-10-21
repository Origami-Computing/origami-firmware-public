#include "hal/fuel_gauge_workaround.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>
#include "core/battery_estimator.h"
#include "hal/hal_power.h"

#ifndef FUEL_GAUGE_HOST_WORKAROUND
#define FUEL_GAUGE_HOST_WORKAROUND 0
#endif

namespace hal {
namespace power {
namespace {
core::battery::Estimator estimator;
bool loaded = false, enabled = FUEL_GAUGE_HOST_WORKAROUND, watch = false;
bool storageOk = true, restorePending = false;
uint32_t lastSaveMs = 0, lastPrintMs = 0, lastHistoryMs = 0;
uint8_t lastPrintedPercent = 255;
FuelGaugeTelemetry latest{};
bool haveLatest = false;

// Single versioned, checksummed NVS record avoids partially updated settings.
struct Record {
    uint32_t magic, enabled, capacityMah;
    int32_t scaleMilli;
    uint32_t remainingUah, voltageMv, timestamp, hasEstimate, checksum;
};
Record saved{};
uint32_t checksum(const Record& record) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t result = 2166136261u;
    for (unsigned i = 0; i < sizeof(record) - sizeof(record.checksum); ++i)
        result = (result ^ bytes[i]) * 16777619u;
    return result;
}

struct History {
    uint32_t uptimeMs;
    uint16_t mv, rawPercent;
    int16_t rawMa;
    int32_t correctedMa;
    uint32_t remainingUah;
    uint8_t percent;
};
constexpr unsigned kHistorySize = 120; // Ten minutes at one sample per five seconds.
History history[kHistorySize]{};
unsigned historyHead = 0, historyCount = 0;

void load() {
    if (loaded) return;
    loaded = true;
    Preferences preferences;
    // Read/write creates the namespace on first use without a missing-NVS error.
    storageOk = preferences.begin("fuel-host", false);
    if (storageOk) {
        if (preferences.getBytesLength("state") == sizeof(saved) &&
            preferences.getBytes("state", &saved, sizeof(saved)) == sizeof(saved) &&
            saved.magic == 0x46474831 && saved.checksum == checksum(saved) && saved.enabled <= 1 &&
            saved.capacityMah <= 20000 &&
            core::battery::validConfig({(uint16_t)saved.capacityMah, saved.scaleMilli})) {
            enabled = saved.enabled != 0;
            estimator.configure({(uint16_t)saved.capacityMah, saved.scaleMilli});
            const auto reason = esp_reset_reason();
            // Cold boots/deep sleep may hide arbitrary battery use or replacement.
            // Re-anchor those from voltage, never integrate an unknown interval.
            restorePending = saved.hasEstimate == 1 &&
                saved.remainingUah <= saved.capacityMah * 1000u &&
                (reason == ESP_RST_SW || reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                 reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);
        } else if (preferences.getBytesLength("state") != 0) {
            Serial.println("[fuel-host] ignoring invalid saved state; using build defaults");
        }
        preferences.end();
    }
    Serial.printf("[fuel-host] mode=%s capacity=%u mAh scale=%.3f provisional=1 storage=%s\n",
                  enabled ? "host" : "gauge", estimator.config().capacityMah,
                  estimator.config().scaleMilli / 1000.0, storageOk ? "ok" : "failed");
}

bool save() {
    Record record{};
    record.magic = 0x46474831;
    record.enabled = enabled ? 1 : 0;
    record.capacityMah = estimator.config().capacityMah;
    record.scaleMilli = estimator.config().scaleMilli;
    record.remainingUah = (uint32_t)lround(estimator.remainingMah() * 1000.0);
    record.voltageMv = latest.voltage_mv;
    const time_t now = time(nullptr);
    record.timestamp = now >= 1704067200 ? (uint32_t)now : 0;
    record.hasEstimate = fuelWorkaroundPercent() <= 100 ? 1 : 0;
    record.checksum = checksum(record);
    Preferences preferences;
    storageOk = preferences.begin("fuel-host", false);
    if (storageOk) {
        storageOk = preferences.putBytes("state", &record, sizeof(record)) == sizeof(record);
        preferences.end();
    }
    if (!storageOk) Serial.println("[fuel-host] ERROR: NVS save failed; RAM settings remain active");
    lastSaveMs = millis();
    return storageOk;
}

void print() {
    Serial.printf("[fuel-host] mode=%s SOC=%u%% remaining=%.2f/%u mAh current=%.2f mA "
                  "raw_SOC=%u%% raw_current=%d mA voltage=%u mV scale=%.3f "
                  "anchor=%s unobserved=%u valid=%u storage=%s\n",
                  enabled ? "host" : "gauge", fuelWorkaroundPercent(), estimator.remainingMah(),
                  estimator.config().capacityMah, estimator.currentMa(), latest.percent,
                  latest.current_ma, latest.voltage_mv, estimator.config().scaleMilli / 1000.0,
                  core::battery::anchorName(estimator.anchor()), estimator.unobserved() ? 1 : 0,
                  fuelWorkaroundPercent() <= 100 ? 1 : 0, storageOk ? "ok" : "failed");
}

void clearHistory() { historyHead = historyCount = 0; lastHistoryMs = 0; }
void seedFromLatest() {
    // Configuration commands invalidate old accounting and start at the latest
    // usable voltage. No additional I2C reads or gauge configuration writes.
    if (haveLatest) updateFuelWorkaround(latest);
}
} // namespace

bool fuelWorkaroundEnabled() { load(); return enabled; }

void updateFuelWorkaround(const FuelGaugeTelemetry& sample) {
    load();
    latest = sample;
    haveLatest = true;
    const bool present = (sample.battery_status & (1u << 3)) != 0;
    const bool usable = present && (sample.operation_status & (1u << 5)) &&
                        !(sample.operation_status & ((1u << 10) | 1u));
    if (!present) { estimator.clear(); restorePending = false; }
    if (restorePending && usable) {
        restorePending = false;
        const time_t now = time(nullptr);
        const bool recent = !saved.timestamp || now < 1704067200 ||
            (now >= saved.timestamp && now - saved.timestamp <= 600);
        if (recent && abs((int)sample.voltage_mv - (int)saved.voltageMv) <= 100)
            estimator.restore(saved.remainingUah / 1000.0);
    }
    estimator.update(sample.sampled_ms, sample.voltage_mv, sample.current_ma,
                     usable && (uint32_t)(millis() - sample.sampled_ms) < 5000u);
    if (!historyCount || (uint32_t)(sample.sampled_ms - lastHistoryMs) >= 5000u) {
        history[historyHead] = {sample.sampled_ms, sample.voltage_mv, sample.percent,
            sample.current_ma, (int32_t)lround(estimator.currentMa()),
            (uint32_t)lround(estimator.remainingMah() * 1000.0), fuelWorkaroundPercent()};
        historyHead = (historyHead + 1) % kHistorySize;
        if (historyCount < kHistorySize) ++historyCount;
        lastHistoryMs = sample.sampled_ms;
    }
    const uint8_t percent = fuelWorkaroundPercent();
    if (watch || percent != lastPrintedPercent || (uint32_t)(sample.sampled_ms - lastPrintMs) >= 30000u) {
        print();
        lastPrintedPercent = percent;
        lastPrintMs = sample.sampled_ms;
    }
    // At most one automatic checkpoint per five minutes; commands save explicitly.
    if (estimator.valid() && (uint32_t)(millis() - lastSaveMs) >= 300000u) save();
}

uint8_t fuelWorkaroundPercent() {
    if (!haveLatest || (uint32_t)(millis() - latest.sampled_ms) >= 5000u) return 255;
    return estimator.percent();
}
void printFuelWorkaround() { load(); print(); }
void printFuelHistory() {
    load();
    Serial.printf("[fuel-history] samples=%u interval_ms=5000 scale=%.3f capacity=%u mode=%s\n",
                  historyCount, estimator.config().scaleMilli / 1000.0,
                  estimator.config().capacityMah, enabled ? "host" : "gauge");
    Serial.println("uptime_ms,voltage_mv,raw_current_ma,corrected_current_ma,raw_soc,host_soc,host_remaining_mah");
    for (unsigned i = 0; i < historyCount; ++i) {
        const auto& h = history[(historyHead + kHistorySize - historyCount + i) % kHistorySize];
        Serial.printf("%lu,%u,%d,%ld,%u,%u,%.3f\n", (unsigned long)h.uptimeMs, h.mv,
                      h.rawMa, (long)h.correctedMa, h.rawPercent, h.percent, h.remainingUah / 1000.0);
        if ((i % 16) == 0) delay(1);
    }
    Serial.println("[fuel-history] end");
}
bool setFuelWorkaroundMode(bool value) {
    load(); enabled = value; return save();
}
bool setFuelWorkaroundScale(double scale) {
    load();
    if (!isfinite(scale) || fabs(scale) < 0.1 || fabs(scale) > 20.0) return false;
    auto config = estimator.config();
    config.scaleMilli = (int32_t)lround(scale * 1000);
    if (!estimator.configure(config)) return false;
    restorePending = false;
    clearHistory(); seedFromLatest(); return save();
}
bool setFuelWorkaroundCapacity(unsigned capacityMah) {
    load();
    if (capacityMah < 100 || capacityMah > 20000) return false;
    auto config = estimator.config(); config.capacityMah = (uint16_t)capacityMah;
    estimator.configure(config);
    restorePending = false;
    clearHistory(); seedFromLatest(); return save();
}
bool setFuelWorkaroundPercent(double percent) {
    load();
    if (!estimator.setPercent(percent)) return false;
    restorePending = false;
    clearHistory(); seedFromLatest(); return save();
}
bool resetFuelWorkaroundEstimate() {
    load(); estimator.clear(); restorePending = false;
    clearHistory(); seedFromLatest(); return save();
}
void setFuelWorkaroundWatch(bool value) { load(); watch = value; }
void saveFuelWorkaroundBeforeSleep() { load(); save(); }

} // namespace power
} // namespace hal
