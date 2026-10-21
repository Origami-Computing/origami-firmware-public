#pragma once
#include <stdint.h>

namespace core { namespace alert_vibration {
constexpr uint8_t kPresetCount=10, kCustomCount=4, kOff=14, kCount=15, kMaxPulses=12;
struct Pulse { uint16_t onMs, offMs; };
struct Pattern {
  uint8_t version, count;
  char name[21];
  Pulse pulses[kMaxPulses];
  uint16_t leadMs;
};
uint8_t validId(uint8_t id);
const Pattern& pattern(uint8_t id);
const char* name(uint8_t id);
uint32_t cycleMs(const Pattern& value);
bool valid(const Pattern& value);
bool activeAt(const Pattern& value,uint32_t elapsedMs);
bool saveCustom(uint8_t slot,const Pattern& value);
} }
