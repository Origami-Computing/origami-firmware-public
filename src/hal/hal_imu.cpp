#include "hal/hal_imu.h"

#include <Wire.h>
#include <stdlib.h>

#include "Pins.h"

namespace hal {
namespace imu {

static LSM6DSLSensor* s_imu        = nullptr;
static bool           s_pedoEnabled = false;
static uint8_t        s_gyroUsers   = 0;

// ── Hardware single-tap (TAP_SRC 0x1C) ───────────────────────────────────────
// Threshold: 5-bit, 1 LSB = FS/2^5. At FS=2g: 1 LSB = 62.5 mg.
// ShockTime: max tap pulse width  (1 LSB = 8/ODR; at 416 Hz ≈ 19 ms per LSB).
// QuietTime: lockout after tap    (1 LSB = 4/ODR; at 416 Hz ≈ 10 ms per LSB).
static constexpr uint8_t  kTapThreshold  = 0x0C;  // ~750 mg — requires firm knock, not shake
static constexpr uint8_t  kTapShockTime  = 0x02;  // ~38 ms — 0x01 (19 ms) was too narrow for real taps
static constexpr uint8_t  kTapQuietTime  = 0x01;
static constexpr uint32_t kTapDebounceMs = 300;
static uint32_t s_tapLastEventMs = 0;

// ── Hardware wake-up (WAKE_UP_SRC 0x1B, INT1 = GPIO IMU_INT1_PIN) ────────────
// Threshold: 6-bit, 1 LSB = FS/2^6. At FS=2g: 1 LSB ≈ 31.25 mg.
// WakeUpDur: consecutive samples above threshold required before INT1 fires.
static constexpr uint8_t kWakeUpThreshold = 0x08;  // ~250 mg — catches a deliberate shake; 0x10 (~500 mg) was
                                                   // too stiff (a firm shake on battery would not wake the device).
                                                   // Brief footstep spikes are still rejected by kWakeUpDur below.
static constexpr uint8_t kWakeUpDur       = 0x02;  // 3 samples (~7 ms at 416 Hz) — rejects footstep impulses

bool init() {
  if (s_imu) return true;
  // Rev-4 assembled N16R8 hardware places the LSM6DSL on the accessory bus
  // (GPIO4/5), and SA0 is high so it answers at 0x6B. The rail is not usable
  // until board_bringup enables VSYS; services::init() calls us afterward.
  // Board bring-up owns Wire1 because the IMU shares accessory I2C with the
  // codec and camera. Do not end/restart the controller here.
  Serial.printf("[imu] pre-init bus GPIO%d/%d SDA=%d SCL=%d\n",
                ACC_SDA_PIN, ACC_SCL_PIN,
                digitalRead(ACC_SDA_PIN), digitalRead(ACC_SCL_PIN));
  Wire1.setClock(100000);
  Wire1.setTimeOut(50);

  // Fail quickly and with a useful trace if power or routing is wrong. The
  // third-party driver ignores I2C write errors and otherwise spends tens of
  // seconds walking its entire init sequence against a missing address.
  uint8_t txErr = 4;
  uint8_t got = 0;
  uint8_t whoAmI = 0;
  for (uint8_t attempt = 0; attempt < 3 && whoAmI != 0x6A; attempt++) {
    Wire1.beginTransmission(0x6B);
    Wire1.write((uint8_t)0x0F);
    txErr = Wire1.endTransmission(false);
    got = txErr == 0 ? Wire1.requestFrom((uint8_t)0x6B, (uint8_t)1) : 0;
    whoAmI = got == 1 ? (uint8_t)Wire1.read() : 0;
    if (whoAmI != 0x6A) delay(10);
  }
  Serial.printf("[imu] probe addr=0x6B tx=%u rx=%u whoami=0x%02X lines=%d/%d\n",
                txErr, got, whoAmI,
                digitalRead(ACC_SDA_PIN), digitalRead(ACC_SCL_PIN));
  if (txErr != 0 || got != 1 || whoAmI != 0x6A) return false;

  // The STM32duino API unusually expects an 8-bit address and shifts it
  // internally. Its default HIGH constant is 0xD6, which becomes 7-bit 0x6B.
  s_imu = new LSM6DSLSensor(&Wire1, LSM6DSL_ACC_GYRO_I2C_ADDRESS_HIGH);
  if (!s_imu) return false;
  if (s_imu->begin() != LSM6DSL_STATUS_OK) return false;
  s_imu->Enable_X();
  // Gyro is unused by any active feature; left in power-down to save ~3.8 mA.

  // Pedometer is enabled FIRST so the subsequent tap/wake-up calls (both force
  // ODR → 416 Hz) win the ODR race.  services::init() → enableStepCounter()
  // will see s_pedoEnabled = true and return early, not touching ODR again.
  if (s_imu->Enable_Pedometer() == LSM6DSL_STATUS_OK) {
    // Enable_Pedometer() routes step-detection to INT1 by default, which would
    // wake the device from battery sleep on every step.  Clear it so steps fire
    // INT2 instead.  INT2 is not wired on this PCB; the pulse sinks harmlessly.
    LSM6DSL_ACC_GYRO_W_STEP_DET_on_INT1((void*)s_imu,
                                         LSM6DSL_ACC_GYRO_INT1_PEDO_DISABLED);
    s_pedoEnabled = true;
  }

  // Single-tap: route interrupt to INT2 so INT1 stays dedicated to wake-up.
  // Forces ODR → 416 Hz.
  s_imu->Enable_Single_Tap_Detection(LSM6DSL_INT2_PIN);
  s_imu->Set_Tap_Threshold(kTapThreshold);
  s_imu->Set_Tap_Shock_Time(kTapShockTime);
  s_imu->Set_Tap_Quiet_Time(kTapQuietTime);

  // Wake-up: route to INT1 (IMU_INT1_PIN) for esp_sleep_enable_ext0_wakeup.
  // Forces ODR → 416 Hz, winning the race against pedometer's 26 Hz.
  s_imu->Enable_Wake_Up_Detection(LSM6DSL_INT1_PIN);
  // Require 2 consecutive samples above threshold before declaring a wake event,
  // filtering out single-sample spikes (table bumps, transients).
  LSM6DSL_ACC_GYRO_W_WAKE_DUR((void*)s_imu, kWakeUpDur);
  s_imu->Set_Wake_Up_Threshold(kWakeUpThreshold);

  return true;
}

bool isReady() { return s_imu != nullptr; }

bool acquireGyroscope() {
  if (!s_imu) return false;
  if (s_gyroUsers > 0) {
    ++s_gyroUsers;
    return true;
  }

  // 208 Hz preserves deliberate gesture detail without the current draw of
  // the much faster modes. 1000 dps leaves headroom for a quick bowling swing.
  if (s_imu->Set_G_ODR(208.0f) != LSM6DSL_STATUS_OK ||
      s_imu->Set_G_FS(1000.0f) != LSM6DSL_STATUS_OK ||
      s_imu->Enable_G() != LSM6DSL_STATUS_OK) {
    return false;
  }
  s_gyroUsers = 1;
  Serial.println("[imu] gyro acquired: 208 Hz, 1000 dps");
  return true;
}

void releaseGyroscope() {
  if (!s_imu || s_gyroUsers == 0) return;
  --s_gyroUsers;
  if (s_gyroUsers == 0) {
    s_imu->Disable_G();
    Serial.println("[imu] gyro released");
  }
}

bool isGyroscopeEnabled() { return s_gyroUsers > 0; }

bool readAxes(int32_t accel[3], int32_t gyro[3]) {
  if (!s_imu) return false;
  if (s_imu->Get_X_Axes(accel) != LSM6DSL_STATUS_OK) return false;
  if (s_imu->Get_G_Axes(gyro)  != LSM6DSL_STATUS_OK) return false;
  return true;
}
bool readAcceleration(int32_t accel[3]) {
  if (!s_imu) return false;
  // The bundled STM32duino IO_Read reports success even on short I2C reads.
  // Check both transactions so the UI can back off without ingesting garbage.
  // CTRL1_XL FS bits: 00=2g, 01=16g, 10=4g, 11=8g (datasheet section 9.11).
  auto read = [](uint8_t reg, uint8_t* bytes, uint8_t count) {
    Wire1.beginTransmission(0x6B);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom((uint8_t)0x6B, count) != count) return false;
    for (uint8_t i=0;i<count;++i) bytes[i]=(uint8_t)Wire1.read();
    return true;
  };
  uint8_t control=0,raw[6]={};
  if (!read(0x10,&control,1) || !read(0x28,raw,6)) return false;
  static const float sensitivity[]={.061f,.488f,.122f,.244f};
  const float scale=sensitivity[(control>>2)&3];
  for (unsigned i=0;i<3;++i) {
    int16_t value=(int16_t)((uint16_t)raw[i*2]|((uint16_t)raw[i*2+1]<<8));
    accel[i]=(int32_t)(value*scale);
  }
  return true;
}

// Gyro FIFO (LSM6DSL datasheet section 9: FIFO_CTRL3 0x08, FIFO_CTRL5 0x0A, FIFO_STATUS1-4 0x3A-0x3D,
// FIFO_DATA_OUT 0x3E/0x3F, which rolls back to 0x3E on multi-byte reads). Raw Wire1 access like readAcceleration().
static bool fifoRead(uint8_t reg, uint8_t* bytes, uint8_t count) {
  Wire1.beginTransmission(0x6B);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((uint8_t)0x6B, count) != count) return false;
  for (uint8_t i = 0; i < count; ++i) bytes[i] = (uint8_t)Wire1.read();
  return true;
}

static bool fifoWrite(uint8_t reg, uint8_t value) {
  Wire1.beginTransmission(0x6B);
  Wire1.write(reg);
  Wire1.write(value);
  return Wire1.endTransmission() == 0;
}

bool startGyroFifo() {
  if (!s_imu || s_gyroUsers == 0) return false;
  // Bypass (clears it), gyro in the FIFO undecimated with no accel, then continuous mode at 104 Hz.
  return fifoWrite(0x0A, 0x00) && fifoWrite(0x08, 0x08) && fifoWrite(0x0A, (0x4 << 3) | 0x6);
}

void stopGyroFifo() {
  if (!s_imu) return;
  fifoWrite(0x0A, 0x00);
  fifoWrite(0x08, 0x00);
}

int readGyroFifo(int32_t outMdps[][3], int maxSamples) {
  if (!s_imu) return -1;
  uint8_t st[4] = {};
  if (!fifoRead(0x3A, st, 4)) return -1;
  unsigned words = st[0] | ((unsigned)(st[1] & 0x07) << 8);
  const unsigned pattern = st[2] | ((unsigned)(st[3] & 0x03) << 8);
  if (words / 3 > 240) {                            // > 2.3 s stale (the app was paused): start fresh
    fifoWrite(0x0A, 0x00);
    fifoWrite(0x0A, (0x4 << 3) | 0x6);
    return 0;
  }
  uint8_t raw[48];
  if (pattern % 3) {                                // realign to an X word
    const unsigned skip = 3 - pattern % 3;
    if (skip > words) return 0;
    if (!fifoRead(0x3E, raw, (uint8_t)(skip * 2))) return -1;
    words -= skip;
  }
  if (words / 3 > (unsigned)maxSamples) {           // a stalled frame: drop the oldest, keep the newest
    unsigned drop = (words / 3 - (unsigned)maxSamples) * 3;
    Serial.printf("[imu] gyro fifo backlog: dropped %u oldest samples\n", drop / 3);
    while (drop) {
      const unsigned take = drop > 24 ? 24 : drop;
      if (!fifoRead(0x3E, raw, (uint8_t)(take * 2))) return -1;
      drop -= take;
      words -= take;
    }
  }
  int n = 0;
  while (words >= 3 && n < maxSamples) {
    unsigned take = words / 3;
    if (take > 8) take = 8;
    if (take > (unsigned)(maxSamples - n)) take = maxSamples - n;
    if (!fifoRead(0x3E, raw, (uint8_t)(take * 6))) return n ? n : -1;
    for (unsigned i = 0; i < take; ++i, ++n)
      for (unsigned a = 0; a < 3; ++a)
        outMdps[n][a] = (int32_t)(int16_t)((uint16_t)raw[i * 6 + a * 2] | ((uint16_t)raw[i * 6 + a * 2 + 1] << 8)) * 35;
    words -= take * 3;                              // 35 mdps/LSB at 1000 dps full scale
  }
  return n;
}

bool setAccelDataReadyInterrupt(bool enabled) {
  if (!s_imu) return false;
  return LSM6DSL_ACC_GYRO_W_DRDY_XL_on_INT1(
             (void*)s_imu,
             enabled ? LSM6DSL_ACC_GYRO_INT1_DRDY_XL_ENABLED
                     : LSM6DSL_ACC_GYRO_INT1_DRDY_XL_DISABLED) == MEMS_SUCCESS;
}

// ── Step counter ─────────────────────────────────────────────────────────────

bool enableStepCounter() {
  if (!s_imu) return false;
  if (s_pedoEnabled) return true;
  // Fallback: init() failed to enable pedometer (rare).  Enable_Pedometer()
  // forces ODR → 26 Hz, so restore 416 Hz afterward for wake-up detection.
  if (s_imu->Enable_Pedometer() != LSM6DSL_STATUS_OK) return false;
  LSM6DSL_ACC_GYRO_W_STEP_DET_on_INT1((void*)s_imu,
                                       LSM6DSL_ACC_GYRO_INT1_PEDO_DISABLED);
  s_imu->Set_X_ODR(416.0f);
  s_pedoEnabled = true;
  return true;
}

bool disableStepCounter() {
  if (!s_imu) return false;
  if (!s_pedoEnabled) return true;
  if (s_imu->Disable_Pedometer() != LSM6DSL_STATUS_OK) return false;
  s_pedoEnabled = false;
  return true;
}

bool isStepCounterEnabled() { return s_pedoEnabled; }

bool getStepCount(uint16_t* outSteps) {
  if (!s_imu || !outSteps) return false;
  return s_imu->Get_Step_Counter(outSteps) == LSM6DSL_STATUS_OK;
}

bool resetStepCounter() {
  if (!s_imu) return false;
  return s_imu->Reset_Step_Counter() == LSM6DSL_STATUS_OK;
}

// ── Tap (hardware, TAP_SRC 0x1C) ─────────────────────────────────────────────

bool pollTap(uint32_t nowMs) {
  if (!s_imu) return false;
  if ((uint32_t)(nowMs - s_tapLastEventMs) < kTapDebounceMs) return false;
  uint8_t src = 0;
  if (s_imu->ReadReg(LSM6DSL_ACC_GYRO_TAP_SRC, &src) != LSM6DSL_STATUS_OK) return false;
  if (!(src & 0x20)) return false;  // SINGLE_TAP = bit 5
  s_tapLastEventMs = nowMs;
  return true;
}

// ── Wake-up (hardware, WAKE_UP_SRC 0x1B) ─────────────────────────────────────

bool pollWakeUp() {
  if (!s_imu) return false;
  uint8_t src = 0;
  if (s_imu->ReadReg(LSM6DSL_ACC_GYRO_WAKE_UP_SRC, &src) != LSM6DSL_STATUS_OK) return false;
  return (src & 0x08) != 0;  // WU_IA = bit 3
}

int wakeUpIntPin() { return IMU_INT1_PIN; }

}  // namespace imu
}  // namespace hal
