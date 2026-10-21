/*
  ESP32_OV5640_AF.h - OV5640 Auto Focus helper for ESP32 (esp32-camera)

  Originally by Eric Nam (2021), public domain.
  Vendored into origami_firmware and trimmed:
    * Polling loops capped at 600 ms (was 5 s) to avoid stalling the camera
      init path / tripping the task watchdog when the AF firmware refuses
      to come up (e.g. on modules without a VCM).
    * `focusInit()` / `autoFocusMode()` return 2 on timeout so the caller
      can log + continue with fixed focus instead of bailing the whole
      camera bring-up.

  All SCCB writes go through the esp_camera sensor_t function pointers, so
  the camera driver must already be initialised and the I2C bus must still
  be owned by esp_camera (i.e. call this BEFORE handing Wire back to
  Arduino in hal_camera::init()).
*/

#ifndef ESP32_OV5640_AF_h
#define ESP32_OV5640_AF_h

#include <Arduino.h>
#include "ESP32_OV5640_cfg.h"
#include "esp_camera.h"

class OV5640 {
 private:
  sensor_t* sensor;
  bool isOV5640;

 public:
  OV5640();
  // Probes CHIPID_HIGH/LOW; returns true iff sensor is OV5640.
  bool start(sensor_t* _sensor);
  // Loads the ~4.6 kB AF firmware blob into the sensor MCU and waits for
  // the firmware to report S_IDLE. Returns 0 on success, 2 on timeout,
  // -1 if start() never identified an OV5640.
  uint8_t focusInit();
  // Switches the loaded firmware into continuous-AF mode. Same return
  // convention as focusInit().
  uint8_t autoFocusMode();
  // Fires one AF cycle (the lens settles, then locks). Useful as a fallback
  // when continuous mode times out (rare, but happens on some VCMs that
  // refuse to ACK the "relax motor" handshake).
  uint8_t singleShotAutoFocus();
  uint8_t getFWStatus();
};

#endif
