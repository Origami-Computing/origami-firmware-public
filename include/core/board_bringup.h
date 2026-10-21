#pragma once

#include <Arduino.h>

namespace core {
namespace board {

// IO expander → regulators → display → SD card → standard folders → IMU.
// Does not allocate app buffers, start WiFi, or draw UI.
bool bringupMenuHardware(bool shippingWakeAuthorized = false);

}  // namespace board
}  // namespace core
