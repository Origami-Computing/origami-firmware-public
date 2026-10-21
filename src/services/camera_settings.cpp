#include "services/camera_settings.h"
#include "core/store.h"

namespace services { namespace camera {
namespace {
static constexpr const char* kPrefsNs = "camera";
static constexpr const char* kKeyMode = "mode";
static constexpr const char* kKeyAf = "af";
static constexpr const char* kKeyCapture = "cap";
static constexpr const char* kKeyManualAe = "m_ae";
static constexpr const char* kKeyManualGain = "m_gain";
static constexpr const char* kKeyManualWb = "m_wb";
static constexpr const char* kKeyManualSat = "m_sat";
static constexpr const char* kKeyManualContrast = "m_con";
static constexpr const char* kKeyManualSharp = "m_sharp";

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static uint8_t encodeSigned(int8_t v) {
  return (uint8_t)((int)v + 128);
}

static int8_t decodeSigned(uint8_t v, int lo, int hi) {
  return (int8_t)clampInt((int)v - 128, lo, hi);
}
} // namespace

hal::camera::Tuning Settings::tuning() const {
  hal::camera::Tuning t{};
  switch (mode) {
    case Mode::Bright:
      t.ae_level = -2;
      t.gainceiling_idx = 0;
      t.wb_mode = 1;
      t.saturation = -2;
      t.contrast = 0;
      t.sharpness = 0;
      break;
    case Mode::Dark:
      t.ae_level = 1;
      t.gainceiling_idx = 2;
      t.wb_mode = 0;
      t.saturation = 0;
      t.contrast = -1;
      t.sharpness = 0;
      break;
    case Mode::Manual:
      t = manual;
      break;
    case Mode::Default:
    default:
      t.ae_level = 0;
      t.gainceiling_idx = 0;
      t.wb_mode = 2;
      t.saturation = -1;
      t.contrast = -1;
      t.sharpness = 0;
      break;
  }
  t.autofocus = autofocus;
  t.singleShotAf = false;
  return t;
}

void Settings::load() {
  mode = (Mode)clampInt(core::store::getU8(kPrefsNs, kKeyMode, (uint8_t)Mode::Default), 0, (int)Mode::Manual);
  autofocus = core::store::getBool(kPrefsNs, kKeyAf, true);
  capture = (CaptureResolution)clampInt(core::store::getU8(kPrefsNs, kKeyCapture, (uint8_t)CaptureResolution::Vga), 0, (int)CaptureResolution::FiveMp);
  manual.ae_level = decodeSigned(core::store::getU8(kPrefsNs, kKeyManualAe, encodeSigned(0)), -2, 2);
  manual.gainceiling_idx = (uint8_t)clampInt(core::store::getU8(kPrefsNs, kKeyManualGain, 1), 0, 3);
  manual.wb_mode = (uint8_t)clampInt(core::store::getU8(kPrefsNs, kKeyManualWb, 0), 0, 4);
  manual.saturation = decodeSigned(core::store::getU8(kPrefsNs, kKeyManualSat, encodeSigned(0)), -2, 2);
  manual.contrast = decodeSigned(core::store::getU8(kPrefsNs, kKeyManualContrast, encodeSigned(0)), -2, 2);
  manual.sharpness = decodeSigned(core::store::getU8(kPrefsNs, kKeyManualSharp, encodeSigned(0)), -3, 3);
}

bool Settings::save(Setting field) const {
  switch (field) {
    case Setting::Mode: return core::store::putU8(kPrefsNs, kKeyMode, (uint8_t)mode);
    case Setting::Autofocus: return core::store::putBool(kPrefsNs, kKeyAf, autofocus);
    case Setting::Capture: return core::store::putU8(kPrefsNs, kKeyCapture, (uint8_t)capture);
    case Setting::Exposure: return core::store::putU8(kPrefsNs, kKeyManualAe, encodeSigned(manual.ae_level));
    case Setting::Gain: return core::store::putU8(kPrefsNs, kKeyManualGain, manual.gainceiling_idx);
    case Setting::WhiteBalance: return core::store::putU8(kPrefsNs, kKeyManualWb, manual.wb_mode);
    case Setting::Saturation: return core::store::putU8(kPrefsNs, kKeyManualSat, encodeSigned(manual.saturation));
    case Setting::Contrast: return core::store::putU8(kPrefsNs, kKeyManualContrast, encodeSigned(manual.contrast));
    case Setting::Sharpness: return core::store::putU8(kPrefsNs, kKeyManualSharp, encodeSigned(manual.sharpness));
  }
  return false;
}
} }
