#include "services/doodle_controls.h"
#include "core/store.h"

namespace services { namespace doodle_controls {
namespace {
static constexpr games::imu_roll::Tuning kDefaultTuning;
static constexpr char kPrefsNs[] = "doodle";
static constexpr char kKeyVersion[] = "ver";
static constexpr char kKeySpeed10[] = "speed10";
static constexpr char kKeyZero10[] = "zero10";
static constexpr char kKeySoft10[] = "soft10";
static constexpr char kKeyFraction1000[] = "frac1000";
static constexpr char kKeySign[] = "sign";
static constexpr uint8_t kPrefsVersion = 1;

static games::imu_roll::Tuning s_rollTuning;
static bool s_tuningLoaded = false;

static bool isValidMaxSpeed(float pxPerSec) {
  return pxPerSec >= 20.0f && pxPerSec <= 800.0f;
}

static bool isValidDeadZonePair(float zeroDeadZoneDeg, float softDeadZoneDeg) {
  return zeroDeadZoneDeg >= 0.0f &&
         zeroDeadZoneDeg <= 20.0f &&
         softDeadZoneDeg >= zeroDeadZoneDeg + 0.5f &&
         softDeadZoneDeg < s_rollTuning.maxRollDeg;
}

static bool isValidSoftZoneFraction(float fraction) {
  return fraction >= 0.0f && fraction <= 0.75f;
}

static uint16_t floatToTenths(float value) {
  return (uint16_t)(value * 10.0f + 0.5f);
}

static uint16_t floatToThousandths(float value) {
  return (uint16_t)(value * 1000.0f + 0.5f);
}


}  // namespace

const games::imu_roll::Tuning& tuning() { return s_rollTuning; }

bool setTuning(float maxSpeedPxPerSec, float zeroDeadZoneDeg,
                               float softDeadZoneDeg, float softZoneSpeedFraction) {
  if (!isValidMaxSpeed(maxSpeedPxPerSec) ||
      !isValidDeadZonePair(zeroDeadZoneDeg, softDeadZoneDeg) ||
      !isValidSoftZoneFraction(softZoneSpeedFraction)) {
    return false;
  }
  s_rollTuning.maxSpeedPxPerSec = maxSpeedPxPerSec;
  s_rollTuning.zeroDeadZoneDeg = zeroDeadZoneDeg;
  s_rollTuning.softDeadZoneDeg = softDeadZoneDeg;
  s_rollTuning.softZoneSpeedFraction = softZoneSpeedFraction;
  return true;
}

LoadResult load() {
  if (s_tuningLoaded) return LoadResult::Unchanged;
  s_tuningLoaded = true;
  if (core::store::getU8(kPrefsNs, kKeyVersion, 0) != kPrefsVersion) {
    return LoadResult::Unchanged;
  }

  const float speed = (float)core::store::getU16(
      kPrefsNs, kKeySpeed10, floatToTenths(kDefaultTuning.maxSpeedPxPerSec)) / 10.0f;
  const float zero = (float)core::store::getU16(
      kPrefsNs, kKeyZero10, floatToTenths(kDefaultTuning.zeroDeadZoneDeg)) / 10.0f;
  const float soft = (float)core::store::getU16(
      kPrefsNs, kKeySoft10, floatToTenths(kDefaultTuning.softDeadZoneDeg)) / 10.0f;
  const float fraction = (float)core::store::getU16(
      kPrefsNs, kKeyFraction1000,
      floatToThousandths(kDefaultTuning.softZoneSpeedFraction)) / 1000.0f;
  if (setTuning(speed, zero, soft, fraction)) {
    s_rollTuning.sign = core::store::getU8(kPrefsNs, kKeySign, 1) == 2 ? -1.0f : 1.0f;
    return LoadResult::Loaded;
  } else {
    return LoadResult::Invalid;
  }
}

void reset() {
  s_rollTuning = kDefaultTuning;
}

bool save() {
  bool ok = true;
  ok = core::store::putU8(kPrefsNs, kKeyVersion, kPrefsVersion) && ok;
  ok = core::store::putU16(kPrefsNs, kKeySpeed10,
                           floatToTenths(s_rollTuning.maxSpeedPxPerSec)) && ok;
  ok = core::store::putU16(kPrefsNs, kKeyZero10,
                           floatToTenths(s_rollTuning.zeroDeadZoneDeg)) && ok;
  ok = core::store::putU16(kPrefsNs, kKeySoft10,
                           floatToTenths(s_rollTuning.softDeadZoneDeg)) && ok;
  ok = core::store::putU16(kPrefsNs, kKeyFraction1000,
                           floatToThousandths(s_rollTuning.softZoneSpeedFraction)) && ok;
  ok = core::store::putU8(kPrefsNs, kKeySign,
                          s_rollTuning.sign < 0.0f ? 2 : 1) && ok;
  return ok;
}

bool setMaxSpeed(float pxPerSec) {
  if (!isValidMaxSpeed(pxPerSec)) return false;
  s_rollTuning.maxSpeedPxPerSec = pxPerSec;
  return true;
}

bool setDeadZones(float zeroDeadZoneDeg, float softDeadZoneDeg) {
  if (!isValidDeadZonePair(zeroDeadZoneDeg, softDeadZoneDeg)) return false;
  s_rollTuning.zeroDeadZoneDeg = zeroDeadZoneDeg;
  s_rollTuning.softDeadZoneDeg = softDeadZoneDeg;
  return true;
}

bool setSoftZoneFraction(float fraction) {
  if (!isValidSoftZoneFraction(fraction)) return false;
  s_rollTuning.softZoneSpeedFraction = fraction;
  return true;
}

bool setSign(float sign) {
  if (!isfinite(sign) || sign == 0.0f) return false;
  s_rollTuning.sign = sign < 0.0f ? -1.0f : 1.0f;
  return true;
}


} }  // namespace services::doodle_controls
