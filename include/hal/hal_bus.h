#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Shared bus coordination helpers.
//
// Rules:
// - Only HAL code should use these locks.
// - App code should never call Wire/SPI device drivers directly when a HAL exists.

namespace hal {
namespace bus {

// Returns a global recursive mutex for shared SPI devices (TFT + SD).
SemaphoreHandle_t spiMutex();

// Returns a global mutex for both board I2C controllers. The TCA/fuel gauge use
// Wire; IMU, codec, and camera SCCB use Wire1. Access is serialized because
// some camera lifecycle operations reinstall the accessory-bus controller.
SemaphoreHandle_t i2cMutex();

// Task-context locks backed by permanent static RTOS storage, initialized once.
// No heap allocation. Unexpected static setup failure is fatal rather than
// granting unprotected access; false means the requested take timed out.
bool lockSpi(TickType_t timeout = portMAX_DELAY);
void unlockSpi();
bool lockI2c(TickType_t timeout = portMAX_DELAY);
void unlockI2c();

}  // namespace bus
}  // namespace hal
