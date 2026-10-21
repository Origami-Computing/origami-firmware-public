#pragma once

#include <stdint.h>

namespace core { namespace sleep_timer {

constexpr uint32_t kDefaultSeconds = 60u;
constexpr uint32_t kMinSeconds = 10u;
constexpr uint32_t kMaxSeconds = 3600u;
constexpr int kStepSeconds = 10;

// UI-task owned sleep preferences. Enable reads retain the existing read-through
// default-on policy; failed writes do not publish an optimistic cached value.
bool enabled();
bool setEnabled(bool enabled);

// Cache duration so the idle loop never repeatedly opens NVS for this field.
uint32_t seconds();
uint32_t milliseconds();
bool save(uint32_t seconds);

}}  // namespace core::sleep_timer
