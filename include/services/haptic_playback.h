#pragma once
#include "core/haptic_effect.h"

namespace services { namespace haptic_playback {
// Private execution boundary used by vibration's UI-task owner. One retained
// worker; all calls serialize with its output writes. stop() acknowledges that
// no old effect can drive ON again. Failed OFF writes retry until acknowledged
// or explicitly superseded by stop(true) for a new motor owner. I2C failures are reported, never called success.
struct Status {
  bool playing=false;
  uint32_t maxGapUs=0, transitions=0, highUs=0, elapsedUs=0, errors=0;
};
bool start(const core::haptic::Effect& effect);
// Renewable loop lease: UI must renew within 100 ms; stale animation stops itself.
// maximumMs is 1..10000 and cannot be extended by renewal.
bool startLoop(const core::haptic::Effect& effect,uint32_t maximumMs=4000);
void renewLoop();
void stop(bool handingOff=false);
Status status();
} }
