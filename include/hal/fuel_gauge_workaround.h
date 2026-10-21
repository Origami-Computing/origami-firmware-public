#pragma once

#include <stdint.h>

namespace hal {
namespace power {
struct FuelGaugeTelemetry;

// All functions run on ui_task, like the shared gauge sampler.
bool fuelWorkaroundEnabled();
void updateFuelWorkaround(const FuelGaugeTelemetry& sample);
uint8_t fuelWorkaroundPercent();
void printFuelWorkaround();
void printFuelHistory();
bool setFuelWorkaroundMode(bool enabled);
bool setFuelWorkaroundScale(double scale);
bool setFuelWorkaroundCapacity(unsigned capacityMah);
bool setFuelWorkaroundPercent(double percent);
bool resetFuelWorkaroundEstimate();
void setFuelWorkaroundWatch(bool enabled);
void saveFuelWorkaroundBeforeSleep();
} // namespace power
} // namespace hal
