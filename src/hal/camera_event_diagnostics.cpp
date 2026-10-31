#if defined(ORIGAMI_CAMERA_EVENT_DIAGNOSTICS)
#include "camera_event_diagnostics.h"
#include <Arduino.h>
#include <esp_arduino_version.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/pcnt.h>
#if defined(ORIGAMI_CAMERA_VSYNC_FILTER_BYPASS)
#include <soc/lcd_cam_struct.h>
#endif
#include "Pins.h"

static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate camera event ABI before enabling diagnostics");
namespace {
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
struct Counters {
  uint32_t vsync = 0, dma = 0, maxGapUs = 0, lastUs = 0;
  uint32_t gapsOver45Ms = 0, gapCount = 0, recentGaps[8] = {};
};
Counters counters;
Counters gpioCounters;
bool gpioActive = false;
// Development-only reservation. No production source currently uses PCNT.
constexpr pcnt_unit_t pulseUnit = PCNT_UNIT_0;
bool pulseActive = false;
uint64_t pulseStartedUs = 0;
uint32_t frames = 0, suspicious = 0;

void IRAM_ATTR gpioEdge(void*) {
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  portENTER_CRITICAL_ISR(&mutex);
  if (gpioActive) {
    if (gpioCounters.vsync) {
      const uint32_t gap = now - gpioCounters.lastUs;
      if (gap > gpioCounters.maxGapUs) gpioCounters.maxGapUs = gap;
      if (gap > 45000u) ++gpioCounters.gapsOver45Ms;
      gpioCounters.recentGaps[gpioCounters.gapCount++ & 7u] = gap;
    }
    ++gpioCounters.vsync;
    gpioCounters.lastUs = now;
  }
  portEXIT_CRITICAL_ISR(&mutex);
}
}

void cameraEventBegin() {
#if defined(ORIGAMI_CAMERA_VSYNC_FILTER_BYPASS)
  const unsigned previousFilter = LCD_CAM.cam_ctrl1.cam_vsync_filter_en;
  LCD_CAM.cam_ctrl1.cam_vsync_filter_en = 0;
  LCD_CAM.cam_ctrl.cam_update = 1;
  Serial.printf("[camera-filter-trial] before=%u after=%u threshold=%u\n", previousFilter,
                unsigned(LCD_CAM.cam_ctrl1.cam_vsync_filter_en),
                unsigned(LCD_CAM.cam_ctrl.cam_vsync_filter_thres));
#endif
  pcnt_config_t config{};
  config.pulse_gpio_num = CAMERA_VSYNC_PIN;
  config.ctrl_gpio_num = -1;
  config.lctrl_mode = config.hctrl_mode = PCNT_MODE_KEEP;
  config.pos_mode = PCNT_COUNT_INC;
  config.neg_mode = PCNT_COUNT_DIS;
  config.counter_h_lim = 32767;
  config.counter_l_lim = -32768;
  config.unit = pulseUnit;
  config.channel = PCNT_CHANNEL_0;
  const auto configured = pcnt_unit_config(&config);
  if (configured == ESP_OK) {
    // No PCNT ISR/event ownership: the hardware counts independently of CPU
    // interrupt latency. Keep filtering disabled for this signal observation.
    pulseActive = pcnt_counter_pause(pulseUnit) == ESP_OK &&
                  pcnt_filter_disable(pulseUnit) == ESP_OK &&
                  pcnt_counter_clear(pulseUnit) == ESP_OK &&
                  pcnt_counter_resume(pulseUnit) == ESP_OK;
    if (!pulseActive) pcnt_counter_pause(pulseUnit);
  }
  pulseStartedUs = esp_timer_get_time();
  Serial.printf("[camera-pcnt-probe] active=%u config=%d\n", unsigned(pulseActive), int(configured));
  // Reuse the existing GPIO ISR service (display TE); do not replace it or
  // reconfigure the sensor pin/matrix. LCD_CAM retains its own interrupt.
  const auto pin = static_cast<gpio_num_t>(CAMERA_VSYNC_PIN);
  const auto added = gpio_isr_handler_add(pin, gpioEdge, nullptr);
  if (added != ESP_OK) {
    Serial.printf("[camera-gpio-probe] unavailable=%d\n", int(added));
    return;
  }
  portENTER_CRITICAL(&mutex);
  gpioActive = true;
  portEXIT_CRITICAL(&mutex);
  if (gpio_set_intr_type(pin, GPIO_INTR_POSEDGE) != ESP_OK) {
    portENTER_CRITICAL(&mutex);
    gpioActive = false;
    portEXIT_CRITICAL(&mutex);
    gpio_isr_handler_remove(pin);
    Serial.printf("[camera-gpio-probe] configuration-failed\n");
  } else Serial.printf("[camera-gpio-probe] active pin=%u\n", unsigned(CAMERA_VSYNC_PIN));
}

// Pinned archive: void ll_cam_send_event(cam_obj_t*, cam_event_t, BaseType_t*).
// Event 0 is DMA EOF; event 1 is VSYNC. Observe interrupt arrivals, not queue
// acceptance. Never hold our lock across the SDK's queue operation or its yield.
extern "C" void __real_ll_cam_send_event(void*, int, BaseType_t*);
extern "C" void IRAM_ATTR __wrap_ll_cam_send_event(void* camera, int event,
                                                  BaseType_t* awakened) {
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  portENTER_CRITICAL_ISR(&mutex);
  if (event == 1) {
    if (counters.vsync) {
      const uint32_t gap = now - counters.lastUs;
      if (gap > counters.maxGapUs) counters.maxGapUs = gap;
      if (gap > 45000u) ++counters.gapsOver45Ms;
      counters.recentGaps[counters.gapCount++ & 7u] = gap;
    }
    ++counters.vsync;
    counters.lastUs = now;
  } else if (event == 0) {
    ++counters.dma;
  }
  portEXIT_CRITICAL_ISR(&mutex);
  __real_ll_cam_send_event(camera, event, awakened);
}

void cameraEventObserve(const uint8_t* bytes, size_t size) {
  ++frames;
  // Raw marker counts are a probe, not a JPEG validator: metadata can contain
  // marker-like bytes. Original bytes and timestamps remain untouched.
  unsigned soi = 0, eoi = 0;
  for (size_t i = 1; i < size; ++i) {
    if (bytes[i - 1] != 0xff) continue;
    if (bytes[i] == 0xd8) ++soi;
    if (bytes[i] == 0xd9) ++eoi;
  }
  if (soi <= 1 && eoi <= 1) return;
  ++suspicious;
  if (suspicious > 8) return;
  int16_t pulses = -1;
  const bool pulseRead = pulseActive && pcnt_get_counter_value(pulseUnit, &pulses) == ESP_OK;
  const uint64_t pulseAgeUs = esp_timer_get_time() - pulseStartedUs;
  portENTER_CRITICAL(&mutex);
  const Counters snapshot = counters;
  const Counters gpio = gpioCounters;
  const bool gpioObserved = gpioActive;
  portEXIT_CRITICAL(&mutex);
  Serial.printf("[camera-events] frame=%u bytes=%u soi=%u eoi=%u vsync=%u dma=%u max_gap_us=%u\n",
                frames, unsigned(size), soi, eoi, snapshot.vsync, snapshot.dma,
                snapshot.maxGapUs);
  // Chronological recent intervals, zero-prefixed until eight are available.
  // Snapshot first so formatting never holds the ISR's lock.
  uint32_t gaps[8] = {};
  const unsigned count = snapshot.gapCount < 8 ? snapshot.gapCount : 8;
  for (unsigned i = 0; i < count; ++i)
    gaps[8 - count + i] = snapshot.recentGaps[(snapshot.gapCount - count + i) & 7u];
  Serial.printf("[camera-event-gaps] frame=%u over45ms=%u recent_us=%u,%u,%u,%u,%u,%u,%u,%u\n",
                frames, snapshot.gapsOver45Ms, gaps[0], gaps[1], gaps[2], gaps[3],
                gaps[4], gaps[5], gaps[6], gaps[7]);
  const unsigned gpioCount = gpio.gapCount < 8 ? gpio.gapCount : 8;
  for (unsigned i = 0; i < 8; ++i) gaps[i] = 0;
  for (unsigned i = 0; i < gpioCount; ++i)
    gaps[8 - gpioCount + i] = gpio.recentGaps[(gpio.gapCount - gpioCount + i) & 7u];
  Serial.printf("[camera-gpio-gaps] frame=%u active=%u edges=%u over45ms=%u recent_us=%u,%u,%u,%u,%u,%u,%u,%u\n",
                frames, unsigned(gpioObserved), gpio.vsync, gpio.gapsOver45Ms,
                gaps[0], gaps[1], gaps[2], gaps[3], gaps[4], gaps[5], gaps[6], gaps[7]);
  Serial.printf("[camera-pcnt] frame=%u read=%u pulses=%d elapsed_ms=%u short_window=%u\n",
                frames, unsigned(pulseRead), int(pulses), unsigned(pulseAgeUs / 1000),
                unsigned(pulseAgeUs < 120000000ULL));
}

void cameraEventFinish() {
  // Driver interrupts have been removed and the frame borrower has joined.
  int16_t pulses = -1;
  if (pulseActive) {
    pcnt_counter_pause(pulseUnit);
    pcnt_get_counter_value(pulseUnit, &pulses);
    pcnt_counter_clear(pulseUnit);
  }
  Serial.printf("[camera-pcnt-end] active=%u pulses=%d elapsed_ms=%u\n",
                unsigned(pulseActive), int(pulses), unsigned((esp_timer_get_time() - pulseStartedUs) / 1000));
  pulseActive = false;
  portENTER_CRITICAL(&mutex);
  const bool remove = gpioActive;
  gpioActive = false;
  const Counters gpio = gpioCounters;
  gpioCounters = {};
  portEXIT_CRITICAL(&mutex);
  if (remove) gpio_isr_handler_remove(static_cast<gpio_num_t>(CAMERA_VSYNC_PIN));
  Serial.printf("[camera-gpio-end] active=%u edges=%u max_gap_us=%u over45ms=%u\n",
                unsigned(remove), gpio.vsync, gpio.maxGapUs, gpio.gapsOver45Ms);
  Serial.printf("[camera-events-end] frames=%u suspicious=%u vsync=%u dma=%u max_gap_us=%u over45ms=%u\n",
                frames, suspicious, counters.vsync, counters.dma, counters.maxGapUs,
                counters.gapsOver45Ms);
  counters = {};
  frames = suspicious = 0;
}
#endif
