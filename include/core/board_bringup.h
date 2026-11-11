#pragma once

#include <Arduino.h>

namespace core {
namespace board {

// IO expander → regulators → display → SD card → standard folders → IMU.
// Does not allocate app buffers, start WiFi, or draw UI.
/**
 * Arm the deep-sleep timer toward `targetEpoch`, or record none with 0. Call after
 * clearing wake sources, immediately before deep sleep. Early stages and ignored
 * wakes re-enter sleep during bringup, so only the final stage boots the board.
 */
void armDeepSleepWake(int64_t targetEpoch);

/**
 * Record the non-button expander ports before deep sleep. A tap can be released
 * before bringup reads it, so a wake with these ports unchanged counts as a press.
 */
void rememberSleepInputs(uint8_t port0, uint8_t port2);

bool bringupMenuHardware(bool shippingWakeAuthorized = false);

}  // namespace board
}  // namespace core
