/*
  ESP32_OV5640_AF.cpp - OV5640 Auto Focus helper for ESP32

  Vendored from https://github.com/0015/ESP32-OV5640-AF (public domain),
  with shorter polling timeouts so init failures do not stall the camera
  bring-up path. See header for details.
*/

#include "ESP32_OV5640_AF.h"

// Each polling tick is 5 ms. delay(5) under FreeRTOS is a vTaskDelay that
// yields to IDLE on both cores, so a multi-second cap does NOT starve the
// task watchdog — the only cost is a slow camera init when a module without
// a VCM doesn't ACK. Upstream uses 1000 ticks (= 5 s); we match that for
// the AF-mode handshake (the cold VCM can take 1–2 s to settle on the first
// move) but keep focusInit tighter because that path is purely a firmware
// hello and converges fast or never.
static constexpr uint16_t kFocusInitTicks = 240;   // 1.2 s
static constexpr uint16_t kAfModeTicks    = 1000;  // 5.0 s
static constexpr uint16_t kPollDelayMs    = 5;

OV5640::OV5640() : sensor(nullptr), isOV5640(false) {}

bool OV5640::start(sensor_t* _sensor) {
  sensor = _sensor;
  if (!sensor) {
    isOV5640 = false;
    return false;
  }
  uint8_t vid = sensor->get_reg(sensor, OV5640_CHIPID_HIGH, 0xff);
  uint8_t pid = sensor->get_reg(sensor, OV5640_CHIPID_LOW, 0xff);
  isOV5640 = (vid == 0x56) && (pid == 0x40);
  return isOV5640;
}

uint8_t OV5640::focusInit() {
  if (!isOV5640) return (uint8_t)-1;

  uint16_t addr = 0x8000;
  if (sensor->set_reg(sensor, 0x3000, 0xff, 0x20) < 0) return (uint8_t)-1;

  for (uint16_t i = 0; i < sizeof(OV5640_AF_Config); i++) {
    if (sensor->set_reg(sensor, addr, 0xff, OV5640_AF_Config[i]) < 0) return (uint8_t)-1;
    addr++;
  }

  sensor->set_reg(sensor, OV5640_CMD_MAIN,      0xff, 0x00);
  sensor->set_reg(sensor, OV5640_CMD_ACK,       0xff, 0x00);
  sensor->set_reg(sensor, OV5640_CMD_PARA0,     0xff, 0x00);
  sensor->set_reg(sensor, OV5640_CMD_PARA1,     0xff, 0x00);
  sensor->set_reg(sensor, OV5640_CMD_PARA2,     0xff, 0x00);
  sensor->set_reg(sensor, OV5640_CMD_PARA3,     0xff, 0x00);
  sensor->set_reg(sensor, OV5640_CMD_PARA4,     0xff, 0x00);
  sensor->set_reg(sensor, OV5640_CMD_FW_STATUS, 0xff, 0x7f);
  sensor->set_reg(sensor, 0x3000,               0xff, 0x00);

  uint8_t state = 0x8F;
  for (uint16_t i = 0; i < kFocusInitTicks; i++) {
    state = sensor->get_reg(sensor, 0x3029, 0xff);
    if (state == FW_STATUS_S_IDLE) return 0;
    delay(kPollDelayMs);
  }
  return 2;
}

uint8_t OV5640::autoFocusMode() {
  if (!isOV5640) return (uint8_t)-1;

  sensor->set_reg(sensor, OV5640_CMD_MAIN, 0xff, 0x01);
  sensor->set_reg(sensor, OV5640_CMD_MAIN, 0xff, 0x08);
  for (uint16_t i = 0; i < kAfModeTicks; i++) {
    if (sensor->get_reg(sensor, OV5640_CMD_ACK, 0xff) == 0x00) goto kicked;
    delay(kPollDelayMs);
  }
  return 2;

kicked:
  sensor->set_reg(sensor, OV5640_CMD_ACK,  0xff, 0x01);
  sensor->set_reg(sensor, OV5640_CMD_MAIN, 0xff, AF_CONTINUE_AUTO_FOCUS);
  for (uint16_t i = 0; i < kAfModeTicks; i++) {
    if (sensor->get_reg(sensor, OV5640_CMD_ACK, 0xff) == 0x00) return 0;
    delay(kPollDelayMs);
  }
  return 2;
}

uint8_t OV5640::singleShotAutoFocus() {
  if (!isOV5640) return (uint8_t)-1;
  sensor->set_reg(sensor, OV5640_CMD_ACK,  0xff, 0x01);
  sensor->set_reg(sensor, OV5640_CMD_MAIN, 0xff, AF_TRIG_SINGLE_AUTO_FOCUS);
  for (uint16_t i = 0; i < kAfModeTicks; i++) {
    if (sensor->get_reg(sensor, OV5640_CMD_ACK, 0xff) == 0x00) return 0;
    delay(kPollDelayMs);
  }
  return 2;
}

uint8_t OV5640::getFWStatus() {
  if (!isOV5640) return (uint8_t)-1;
  return sensor->get_reg(sensor, OV5640_CMD_FW_STATUS, 0xff);
}
