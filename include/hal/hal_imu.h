#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <LSM6DSLSensor.h>

namespace hal {
namespace imu {

bool init();
bool isReady();
bool readAxes(int32_t accel[3], int32_t gyro[3]);
bool readAcceleration(int32_t accel[3]);

/** Power the gyroscope for an active motion consumer. Calls are reference
 * counted; every successful acquire must be paired with releaseGyroscope(). */
bool acquireGyroscope();
void releaseGyroscope();
bool isGyroscopeEnabled();
/** Buffer gyro samples in the IMU's FIFO at 104 Hz (needs an acquired gyroscope), so a game that runs at
 *  ~13 fps still sees every sample. readGyroFifo drains up to maxSamples, oldest first, in mdps; the newest is
 *  ~now and they are 1/104 s apart. It returns -1 on a bus error; a backlog over maxSamples keeps the newest
 *  maxSamples (logged), and one over ~2.3 s is reset (0). */
bool startGyroFifo();
int readGyroFifo(int32_t outMdps[][3], int maxSamples);
void stopGyroFifo();

/** Temporarily route accelerometer data-ready to INT1 for an electrical
 *  continuity diagnostic. Disable it afterward to restore wake-only INT1. */
bool setAccelDataReadyInterrupt(bool enabled);

// Hardware pedometer (LSM6DSL Embedded Functions, datasheet §9, EMB_FUNC_EN_A.PEDO_EN).
// Forces XL ODR to 26 Hz / FS=2g internally (see LSM6DSLSensor::Enable_Pedometer).
bool enableStepCounter();
bool disableStepCounter();
bool isStepCounterEnabled();
bool getStepCount(uint16_t* outSteps);
bool resetStepCounter();

/** Hardware single-tap from TAP_SRC (0x1C SINGLE_TAP bit). Call from main loop. */
bool pollTap(uint32_t nowMs);

/** Hardware wake-up from WAKE_UP_SRC (0x1B WU_IA bit). Reading self-clears the flag.
 *  Call once before entering battery sleep to drain any pending event, then use
 *  wakeUpIntPin() with esp_sleep_enable_ext0_wakeup for actual shake-to-wake. */
bool pollWakeUp();

/** GPIO connected to IMU INT1 (active-high when WU_IA fires). Pass to
 *  esp_sleep_enable_ext0_wakeup(gpio_num_t(wakeUpIntPin()), 1). */
int wakeUpIntPin();

}  // namespace imu
}  // namespace hal
