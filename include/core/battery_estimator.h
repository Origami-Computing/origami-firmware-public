#pragma once

#include <math.h>
#include <stdint.h>

namespace core {
namespace battery {

// Provisional single-cell 4.2 V curve, NOT a characterized model of this pack.
// The user-selected full anchor is 4.15 V (formerly the 95% point).
// Used only to establish a starting balance; current integration runs thereafter.
inline uint8_t voltagePercent(uint16_t mv) {
    if (!mv) return 255;
    struct Point { uint16_t mv; uint8_t percent; };
    static constexpr Point points[] = {
        {4150,100}, {4110,90}, {4080,85}, {4020,80},
        {3980,75}, {3950,70}, {3910,65}, {3870,60}, {3850,55},
        {3840,50}, {3820,45}, {3800,40}, {3790,35}, {3770,30},
        {3750,25}, {3730,20}, {3710,15}, {3690,10}, {3610,5}, {3300,0}
    };
    if (mv >= points[0].mv) return 100;
    for (unsigned i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
        if (mv >= points[i].mv) {
            const auto& hi = points[i-1];
            const auto& lo = points[i];
            return lo.percent + (uint32_t)(mv - lo.mv) * (hi.percent - lo.percent) / (hi.mv - lo.mv);
        }
    }
    return 0;
}

struct Config {
    uint16_t capacityMah;
    // -10x assumes reversed SRP/SRN, 1 mOhm, and unchanged 10 mOhm gauge gain.
    int32_t scaleMilli;
    constexpr Config(uint16_t capacity = 1000, int32_t scale = -10000)
        : capacityMah(capacity), scaleMilli(scale) {}
};

inline bool validConfig(const Config& config) {
    const int64_t magnitude = config.scaleMilli < 0 ? -(int64_t)config.scaleMilli : config.scaleMilli;
    return config.capacityMah >= 100 && config.capacityMah <= 20000 &&
           magnitude >= 100 && magnitude <= 20000;
}

enum class Anchor : uint8_t { Voltage, Manual, Restored };
inline const char* anchorName(Anchor anchor) {
    switch (anchor) {
    case Anchor::Manual: return "manual";
    case Anchor::Restored: return "restored";
    default: return "voltage";
    }
}

class Estimator {
public:
    bool configure(const Config& config) {
        if (!validConfig(config)) return false;
        config_ = config;
        clear(); // A new scale/capacity invalidates the previous accounting.
        return true;
    }
    const Config& config() const { return config_; }
    void clear() {
        initialized_ = valid_ = havePrevious_ = haveTimestamp_ = false;
        remainingMah_ = previousCurrent_ = currentMa_ = 0;
        unobserved_ = false;
        anchor_ = Anchor::Voltage;
    }
    bool setPercent(double percent) {
        if (!isfinite(percent) || percent < 0 || percent > 100) return false;
        remainingMah_ = config_.capacityMah * percent / 100.0;
        initialized_ = true;
        havePrevious_ = false;
        haveTimestamp_ = false;
        anchor_ = Anchor::Manual;
        unobserved_ = false;
        return true;
    }
    bool restore(double remainingMah) {
        if (!isfinite(remainingMah) || remainingMah < 0 || remainingMah > config_.capacityMah) return false;
        remainingMah_ = remainingMah;
        initialized_ = true;
        havePrevious_ = false;
        haveTimestamp_ = false;
        anchor_ = Anchor::Restored;
        unobserved_ = true; // Never extrapolate the last current over reset time.
        return true;
    }
    void update(uint32_t now, uint16_t mv, int16_t rawMa, bool usable) {
        currentMa_ = (double)rawMa * config_.scaleMilli / 1000.0;
        if (!usable || mv < 2500 || mv > 4400 || fabs(currentMa_) > 10000) {
            valid_ = false;
            havePrevious_ = false;
            unobserved_ = true;
            return;
        }
        valid_ = true;
        const uint32_t elapsed = now - lastMs_; // millis wrap is intentional.
        if (!initialized_ || (haveTimestamp_ && elapsed > 120000u)) {
            // Loaded/charging voltage is only an approximate initial anchor.
            const uint8_t percent = voltagePercent(mv);
            remainingMah_ = config_.capacityMah * percent / 100.0;
            initialized_ = true;
            anchor_ = Anchor::Voltage;
        } else if (havePrevious_ && elapsed <= 5000u) {
            // Trapezoid integration, mA * milliseconds / 3,600,000 = mAh.
            remainingMah_ += (previousCurrent_ + currentMa_) * 0.5 * elapsed / 3600000.0;
        }
        if (haveTimestamp_ && elapsed > 5000u) unobserved_ = true;
        if (remainingMah_ < 0) remainingMah_ = 0;
        if (remainingMah_ > config_.capacityMah) remainingMah_ = config_.capacityMah;
        previousCurrent_ = currentMa_;
        lastMs_ = now;
        havePrevious_ = true;
        haveTimestamp_ = true;
    }
    bool valid() const { return initialized_ && valid_; }
    bool initialized() const { return initialized_; }
    bool unobserved() const { return unobserved_; }
    Anchor anchor() const { return anchor_; }
    double currentMa() const { return currentMa_; }
    double remainingMah() const { return remainingMah_; }
    uint8_t percent() const {
        if (!valid()) return 255;
        const double percent = remainingMah_ * 100.0 / config_.capacityMah;
        // Floor prevents showing 100% before the accounting reaches full.
        return (uint8_t)(percent + 1e-7);
    }
private:
    Config config_{};
    double remainingMah_ = 0, previousCurrent_ = 0, currentMa_ = 0;
    uint32_t lastMs_ = 0;
    bool initialized_ = false, valid_ = false, havePrevious_ = false, haveTimestamp_ = false, unobserved_ = false;
    Anchor anchor_ = Anchor::Voltage;
};

} // namespace battery
} // namespace core
