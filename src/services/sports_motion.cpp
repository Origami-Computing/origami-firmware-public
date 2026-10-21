// Motion input for the Wii-style games: see include/services/sports_motion.h.
#if defined(MENU_APP) && defined(KAMI_GAMES) && KAMI_GAMES  // N16R8 only (platformio.ini)

#include "services/sports_motion.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "services/motion.h"
#include "core/bias_estimator.h"

namespace services {
namespace motion_input {

namespace {
constexpr int kMax = 40;                 // ~380 ms of samples per frame
services::motion::GyroFifo s_fifo;
Sample* s_out = nullptr;
bool s_open = false;
// calibration
uint32_t s_calStart = 0, s_calHold = 1000;
float s_sum[3], s_max = 0, s_bias[3] = {0, 0, 0}, s_up[3] = {0, 0, 1000};
uint32_t s_n = 0;
int s_last = 0;
core::motion::BiasEstimator s_est;     // the live, per-device offset (rests and still holds refine it)
}  // namespace

bool open() {
  if (s_open) return true;
  if (!s_out) s_out = (Sample*)heap_caps_malloc(sizeof(Sample) * kMax, MALLOC_CAP_SPIRAM);
  if (!s_out || !s_fifo.open()) { close(); return false; }
  s_open = true;
  return true;
}

void close() {
  s_fifo.close();
  heap_caps_free(s_out);
  s_out = nullptr;
  s_open = false;
  s_last = 0;
}

bool isOpen() { return s_open; }

int poll(uint32_t now) {
  s_last = 0;
  if (!s_open) return 0;
  const int n = s_fifo.poll();
  const auto s_raw = s_fifo.samples();
  if (n <= 0) return 0;
  for (int i = 0; i < n; ++i) {
    s_out[i].ms = now - (uint32_t)((n - 1 - i) * 1000 / 104);
    for (int a = 0; a < 3; ++a) s_out[i].dps[a] = s_raw[i][a] / 1000.0f;
    s_est.add(s_out[i].dps);
  }
  s_last = n;
  return n;
}

const Sample* samples() { return s_out; }

void calibrateBegin(uint32_t now, uint16_t holdMs) {
  s_calStart = now;
  s_calHold = holdMs;
  s_sum[0] = s_sum[1] = s_sum[2] = 0;
  s_max = 0;
  s_n = 0;
}

Calibration calibrateStep(uint32_t now) {
  for (int i = 0; i < s_last; ++i) {
    const float* g = s_out[i].dps;
    for (int a = 0; a < 3; ++a) s_sum[a] += g[a];
    const float m = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (m > s_max) s_max = m;
    ++s_n;
  }
  if (s_max > 40.0f) { calibrateBegin(now, s_calHold); return Calibration::Moved; }   // bias is a few dps
  if ((int32_t)(now - s_calStart) < (int32_t)s_calHold || s_n < 20) return Calibration::Busy;
  for (int a = 0; a < 3; ++a) s_bias[a] = s_sum[a] / s_n;
  if (!s_est.valid()) s_est.seed(s_bias);                // a resting estimate (if any) beats a hand-held hold
  float acc[3];
  if (!readAccel(acc)) return Calibration::Busy;
  for (int a = 0; a < 3; ++a) s_up[a] = acc[a];
  return Calibration::Done;
}

const float* bias() { return s_est.valid() ? s_est.bias() : s_bias; }   // live per-device estimate
const float* upMg() { return s_up; }

bool readAccel(float out[3]) {
  int32_t a[3] = {};
  if (!s_fifo.readAcceleration(a)) return false;
  for (int i = 0; i < 3; ++i) out[i] = (float)a[i];
  return true;
}

}  // namespace motion_input
}  // namespace services

#endif  // MENU_APP
