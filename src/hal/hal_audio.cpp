#include "hal/hal_audio.h"
#include "hal/audio_retirement.h"
#include <Arduino.h>
#include <atomic>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Pins.h"
#include "hal/hal_io.h"
#include "sfx/sfx.h"

// Sample rate for microphone
#define MIC_SAMPLE_RATE 48000

namespace hal {
namespace audio {

namespace {
constexpr char kAudioPrefsNs[] = "halaud";
constexpr char kAudioPrefsRouteKey[] = "out";

static OutputRoute outputRouteClampU8(uint8_t v) {
  if (v > static_cast<uint8_t>(OutputRoute::Auto)) return OutputRoute::Both;
  return static_cast<OutputRoute>(v);
}
}  // namespace

static Adafruit_TLV320DAC3100 s_codec;
static bool s_codecInitialized = false;
static bool s_micInstalled = false;
static bool s_txInstalled = false;
static uint32_t s_txGeneration = 0;
static size_t s_txDmaBytes = 0;
static size_t s_txDmaOffset = 0;
static uint32_t s_codecConfigGeneration = 0;

// Codec mode tracker — avoids redundant `configureForPlayback()` calls (each one
// does ~30 ms of I2C register writes; running it
// when the codec is already in playback mode is a pure UI-latency hit).
enum class CodecMode : uint8_t { Unknown, Playback, Recording };
static CodecMode s_codecMode = CodecMode::Unknown;

static bool s_outputRouteLoaded = false;
static bool s_routeApplyDeferred = false;
static OutputRoute s_outputRoute = OutputRoute::Both;
static std::atomic<OutputRoute> s_effectiveOutputRoute{OutputRoute::Both};
static bool s_headsetDetectionReady = false;
static bool s_headphonesPresent = false;
static uint8_t s_headsetCandidate = 0xFF;
static uint32_t s_headsetPollMs = 0;
static uint32_t s_headsetCandidateMs = 0;
static constexpr uint32_t kHeadsetPollMs = 50;
static constexpr uint32_t kHeadsetDebounceMs = 100;

static void outputRouteApplyFromNvs();  // defined below, called from configureForPlayback()
static void applyOutputRoute();

static SemaphoreHandle_t s_i2sMutex = nullptr;
static void ensureI2sMutex() {
  if (!s_i2sMutex) {
    s_i2sMutex = xSemaphoreCreateMutex();
  }
}

static bool lockI2s(TickType_t timeout = portMAX_DELAY) {
  ensureI2sMutex();
  if (!s_i2sMutex) return false;
  return xSemaphoreTake(s_i2sMutex, timeout) == pdTRUE;
}

static void unlockI2s() {
  if (s_i2sMutex) xSemaphoreGive(s_i2sMutex);
}

bool codecReady() {
  return !retirementPending() && s_codecInitialized;
}

void hardwareReset() {
  if (retirementPending()) return;
  s_codecInitialized = false;
  s_headsetDetectionReady = false;
  s_headphonesPresent = false;
  s_headsetCandidate = 0xFF;
  // rev4: TLV320 ~RESET is on expander P12 (AUDIO_RESET_PIN is -1; GPIO21 is now
  // camera Y7 and must NOT be driven here). Pulse ~RESET low then release.
  hal::io::setAudioReset(true);   // assert (LOW)
  delay(10);
  hal::io::setAudioReset(false);  // deassert (HIGH)
  delay(100);
  s_codecMode = CodecMode::Unknown;
}

bool initCodec(uint8_t i2cAddress) {
  if (retirementPending()) return false;
  // Rev 4 routes the codec over accessory I2C (Wire1, GPIO4/5).
  if (!s_codec.begin(i2cAddress, &Wire1)) {
    return false;
  }
  s_codecInitialized = true;
  return true;
}

bool configureForPlayback() {
  if (retirementPending()) return false;
  if (!s_codecInitialized) return false;
  
  bool ok = true;
  
  // Clock configuration - codec derives clock from BCLK
  ok &= s_codec.setCodecInterface(TLV320DAC3100_FORMAT_I2S, TLV320DAC3100_DATA_LEN_16);
  ok &= s_codec.setCodecClockInput(TLV320DAC3100_CODEC_CLKIN_PLL);
  ok &= s_codec.setPLLClockInput(TLV320DAC3100_PLL_CLKIN_BCLK);
  ok &= s_codec.setPLLValues(1, 4, 16, 0);
  ok &= s_codec.setNDAC(true, 2);
  ok &= s_codec.setMDAC(true, 8);
  ok &= s_codec.setDOSR(128);
  ok &= s_codec.powerPLL(true);
  
  // DAC path
  ok &= s_codec.setDACDataPath(true, true, TLV320_DAC_PATH_MIXED,
                               TLV320_DAC_PATH_NORMAL, TLV320_VOLUME_STEP_1SAMPLE);
  ok &= s_codec.configureAnalogInputs(TLV320_DAC_ROUTE_MIXER, TLV320_DAC_ROUTE_MIXER,
                                      false, false, false, false);
  ok &= s_codec.setDACVolumeControl(false, false, TLV320_VOL_INDEPENDENT);
  ok &= s_codec.setChannelVolume(false, 0.0f);  // Left — unity; mixer amp already adds +6 dB
  ok &= s_codec.setChannelVolume(true, 0.0f);   // Right
  
  // Headphone output
  ok &= s_codec.configureHeadphoneDriver(false, false, TLV320_HP_COMMON_1_35V, false);
  ok &= s_codec.configureHPL_PGA(0, true);
  ok &= s_codec.configureHPR_PGA(0, true);
  // Analog volume codes: lower = louder on HPL/HPR/SPK (see Adafruit basicI2Sconfig; 0 ≈ 0 dB on SPK).
  ok &= s_codec.setHPLVolume(true, 4);
  ok &= s_codec.setHPRVolume(true, 4);
  
  // Keep both outputs off until the selected route is applied below.
  ok &= s_codec.enableSpeaker(false);
  ok &= s_codec.configureSPK_PGA(TLV320_SPK_GAIN_6DB, true);
  ok &= s_codec.setSPKVolume(true, 0);

  if (ok) {
    s_codecMode = CodecMode::Playback;
    if (!s_headsetDetectionReady) configureHeadsetDetection();
    outputRouteApplyFromNvs();
    ++s_codecConfigGeneration;
  }
  return ok;
}

bool configureHeadsetDetection() {
  if (retirementPending()) return false;
  s_headsetDetectionReady = false;
  if (!s_codecInitialized) return false;
  // MCLK is unconnected on rev4. TI SLAA540B requires selecting/enabling the
  // internal oscillator before headset detection when MCLK is unavailable.
  // Page 3 / register 16 bit 7 = 0 selects that oscillator.
  if (!s_codec.configDelayDivider(false, 0)) return false;
  // The detector uses MICBIAS to distinguish a 3-pole headphone plug from a
  // 4-pole headset. GPIO1 carries the insertion/removal interrupt to AUDIO.INT.
  s_headsetDetectionReady = s_codec.configMicBias(false, true, TLV320_MICBIAS_AVDD) &&
         s_codec.setHeadsetDetect(true) &&
         s_codec.setInt1Source(true, true, false, false, false, false) &&
         s_codec.setGPIO1Mode(TLV320_GPIO1_INT1);
  s_headsetPollMs = millis();
  s_headsetCandidate = 0xFF;
  return s_headsetDetectionReady;
}

uint8_t headsetStatus() {
  // The library returns NONE on an I2C error. Preserve the previous route on
  // failed reads instead of treating a bus failure as an unplug event.
  uint8_t value = 0;
  if (!readRegister(0, 67, &value)) return 0xFF;
  return (value >> 5) & 0x03;
}

uint8_t headsetIrqFlags(bool clearSticky) {
  if (!s_codecInitialized) return 0;
  return s_codec.readIRQflags(clearSticky);
}

bool readRegister(uint8_t page, uint8_t reg, uint8_t* value) {
  if (!s_codecInitialized || !value) return false;
  Wire1.beginTransmission(0x18);
  Wire1.write((uint8_t)0x00);  // page-select register
  Wire1.write(page);
  if (Wire1.endTransmission(true) != 0) return false;
  Wire1.beginTransmission(0x18);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((int)0x18, (int)1, (int)true) != 1) return false;
  *value = (uint8_t)Wire1.read();
  return true;
}

uint32_t codecConfigGeneration() {
  return s_codecConfigGeneration;
}

static void applyPlaybackAfterMic() {
  if (s_codecInitialized) {
    configureForPlayback();
  } else {
    hardwareReset();
    if (initCodec(0x18)) configureForPlayback();
  }
  sfx::onPlaybackRestored();
}

void endMicSession() {
  if (retirementPending()) return;
  // Fast path: no mic session was actually open and the codec is already in
  // playback mode. The previous unconditional reconfigure cost ~240 ms of
  // blocking I2C on the UI thread for every `restoreForUiPlayback()` call
  // (MP3 stop, agent exit, voice-recorder exit), which is the dominant cause
  // of the "first SFX after navigating feels laggy" symptom in menu use.
  if (!s_micInstalled && s_codecMode == CodecMode::Playback) {
    return;
  }
  deinitMic();
  applyPlaybackAfterMic();
  Serial.println("[hal_audio] endMicSession -> playback (I2S RX off, codec DAC)");
}

void restoreForUiPlayback() {
  if (retirementPending()) return;
  if (s_routeApplyDeferred) applyOutputRoute();
  const bool hadTx = s_txInstalled;
  if (s_txInstalled) txEnd();
  if (s_micInstalled || s_codecMode != CodecMode::Playback) {
    endMicSession();
    return;
  }
  if (hadTx || s_codecInitialized) {
    sfx::onPlaybackRestored();
  }
}

void invalidateForWake() {
  if (retirementPending()) return;
  // Light sleep stopped I2S BCLK, so the BCLK-derived codec PLL lost lock and the
  // DAC config is dead — but the cache (s_codecInitialized / s_codecMode) survived
  // in RAM, so the next playback would skip reconfiguration and be silent. Drop the
  // I2S driver and clear every cache flag so the next sound does a full re-init:
  //   ensurePlaybackCodec(): !codecReady() -> hardwareReset() + initCodec();
  //   bumped codecConfigGeneration -> sfx_voice re-runs configureForPlayback().
  if (s_txInstalled || s_micInstalled) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    s_txInstalled = false;
    s_micInstalled = false;
    ++s_txGeneration;
  }
  s_codecInitialized = false;
  s_headsetDetectionReady = false;
  s_headphonesPresent = false;
  s_headsetCandidate = 0xFF;
  s_codecMode = CodecMode::Unknown;
  ++s_codecConfigGeneration;
}

void setSpeakerEnabled(bool on) {
  if (retirementPending()) return;
  if (!s_codecInitialized) return;
  if (on) {
    s_codec.setSPKVolume(true, 0);
    s_codec.enableSpeaker(true);
    s_codec.configureSPK_PGA(TLV320_SPK_GAIN_6DB, true);
  } else {
    s_codec.enableSpeaker(false);
    s_codec.setSPKVolume(false, 0);  // disconnect SPK from Left Mixer — removes extra load that made HPL quieter than HPR
  }
}

static void enableHeadphonePath(bool on) {
  if (retirementPending()) return;
  if (!s_codecInitialized) return;
  if (on) {
    s_codec.configureHeadphoneDriver(true, true, TLV320_HP_COMMON_1_35V, false);
    s_codec.configureHPL_PGA(0, true);
    s_codec.configureHPR_PGA(0, true);
    s_codec.setHPLVolume(true, 4);
    s_codec.setHPRVolume(true, 4);
  } else {
    s_codec.configureHeadphoneDriver(false, false, TLV320_HP_COMMON_1_35V, false);
  }
}

static void applyOutputRoute() {
  const OutputRoute r = s_outputRoute == OutputRoute::Auto
      ? (s_headphonesPresent ? OutputRoute::HeadphonesOnly : OutputRoute::SpeakerOnly)
      : s_outputRoute;
  s_effectiveOutputRoute.store(r, std::memory_order_release);
  // Jack changes during recording must not power the DAC outputs back up.
  if (retirementPending()) { s_routeApplyDeferred = true; return; }
  if (!s_codecInitialized || s_codecMode != CodecMode::Playback) return;
  s_routeApplyDeferred = false;
  switch (r) {
    case OutputRoute::Both:
      setSpeakerEnabled(true);
      enableHeadphonePath(true);
      break;
    case OutputRoute::SpeakerOnly:
      enableHeadphonePath(false);
      setSpeakerEnabled(true);
      break;
    case OutputRoute::HeadphonesOnly:
      setSpeakerEnabled(false);
      enableHeadphonePath(true);
      break;
    case OutputRoute::Auto:
      break;  // Resolved above.
  }
}

void setOutputRoute(OutputRoute r) {
  // Load once before accepting a session override, including before codec init.
  if (!s_outputRouteLoaded) outputRouteApplyFromNvs();
  s_outputRoute = outputRouteClampU8(static_cast<uint8_t>(r));
  applyOutputRoute();
}

OutputRoute getOutputRoute() { return s_outputRoute; }
OutputRoute getEffectiveOutputRoute() { return s_effectiveOutputRoute.load(std::memory_order_acquire); }

void serviceOutputRoute(uint32_t nowMs) {
  if (retirementPending()) return;
  if (!s_codecInitialized || (uint32_t)(nowMs - s_headsetPollMs) < kHeadsetPollMs) return;
  s_headsetPollMs = nowMs;
  if (!s_headsetDetectionReady) {
    configureHeadsetDetection();
    return;
  }
  const uint8_t status = headsetStatus();
  if (status != 0 && status != 1 && status != 3) {
    s_headsetCandidate = 0xFF;
    return;
  }
  // Both 3-pole headphones and 4-pole headsets select Aux. Require a stable
  // presence reading so insertion chatter cannot repeatedly switch amplifiers.
  const uint8_t present = status != 0;
  if (present != s_headsetCandidate) {
    s_headsetCandidate = present;
    s_headsetCandidateMs = nowMs;
    return;
  }
  if ((uint32_t)(nowMs - s_headsetCandidateMs) < kHeadsetDebounceMs ||
      s_headphonesPresent == (present != 0)) return;
  s_headphonesPresent = present != 0;
  if (s_outputRoute == OutputRoute::Auto) {
    applyOutputRoute();
    Serial.printf("[hal_audio] auto -> %s (jack=%u)\n",
                  s_headphonesPresent ? "aux" : "speaker", status);
  }
}

static void outputRouteApplyFromNvs() {
  if (s_outputRouteLoaded) {
    applyOutputRoute();
    return;
  }
  Preferences p;
  // RW so the namespace is created on first boot; read-only fails NOT_FOUND
  // until something has been written and produces noisy boot-log errors.
  if (!p.begin(kAudioPrefsNs, false)) {
    applyOutputRoute();
    return;
  }
  uint8_t v = p.getUChar(kAudioPrefsRouteKey, static_cast<uint8_t>(OutputRoute::Both));
  p.end();
  s_outputRouteLoaded = true;
  s_outputRoute = outputRouteClampU8(v);
  applyOutputRoute();
}

void outputRouteSaveToNvs(OutputRoute r) {
  r = outputRouteClampU8(static_cast<uint8_t>(r));
  Preferences p;
  if (p.begin(kAudioPrefsNs, false)) {
    p.putUChar(kAudioPrefsRouteKey, static_cast<uint8_t>(r));
    p.end();
  }
  s_outputRouteLoaded = true;
  setOutputRoute(r);
}

bool configureForRecording() {
  if (retirementPending()) return false;
  if (!s_codecInitialized) return false;
  
  // Disable DAC outputs for recording
  s_codec.setDACVolumeControl(true, true, TLV320_VOL_INDEPENDENT);
  s_codec.enableSpeaker(false);
  s_codec.configureHeadphoneDriver(false, false, TLV320_HP_COMMON_1_35V, false);
  s_codec.setDACDataPath(false, false, TLV320_DAC_PATH_NORMAL,
                         TLV320_DAC_PATH_NORMAL, TLV320_VOLUME_STEP_1SAMPLE);
  s_codec.setNDAC(false, 2);
  s_codec.setMDAC(false, 8);
  s_codec.setDOSR(0);
  s_codec.powerPLL(false);

  s_codecMode = CodecMode::Recording;
  ++s_codecConfigGeneration;
  return true;
}

bool initMic(bool streaming) {
  if (retirementPending()) return false;
  if (!lockI2s(pdMS_TO_TICKS(500))) {
    Serial.println("[hal_audio] initMic: lock timeout");
    return false;
  }

  if (s_codecInitialized) configureForRecording();

  // Always reinstall I2S in RX mode: MP3/agent playback may have left it in TX on I2S_NUM_0.
  if (s_micInstalled) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    s_micInstalled = false;
  }
  if (s_txInstalled) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    s_txInstalled = false;
  }

  // I2S config for microphone (ICS-43434)
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // ICS-43434 outputs 24-bit in 32-bit frame
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 0,
    .dma_buf_len = 0,
    .use_apll = true,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_DIN_PIN
  };

  struct MicDmaPlan {
    int count;
    int len;
  };
  static const MicDmaPlan kPlans[] = {
    {8, 512},
    {6, 256},
    {4, 256},
    {4, 128},
  };

  esp_err_t err = ESP_FAIL;
  MicDmaPlan chosen = {0, 0};
  for (const MicDmaPlan& plan : kPlans) {
    // Live voice also needs DMA-capable RAM for TLS. Its early capture service
    // drains every UI tick; use the existing 32 ms profile without first
    // attempting a 16 KB allocation that can starve concurrent encryption.
    if (streaming && plan.count == 8) continue;
    i2s_config.dma_buf_count = plan.count;
    i2s_config.dma_buf_len = plan.len;
    err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, nullptr);
    if (err == ESP_OK) {
      chosen = plan;
      break;
    }
    Serial.printf("[hal_audio] I2S mic install failed: err=%d dma=%dx%d intFree=%u intLargest=%u dmaLargest=%u\n",
                  err, plan.count, plan.len,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  }
  if (err != ESP_OK) {
    unlockI2s();
    return false;
  }

  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[hal_audio] I2S mic pin config failed: %d\n", err);
    i2s_driver_uninstall(I2S_NUM_0);
    unlockI2s();
    return false;
  }

  i2s_start(I2S_NUM_0);
  s_micInstalled = true;
  Serial.printf("[hal_audio] Mic initialized dma=%dx%d\n", chosen.count, chosen.len);
  unlockI2s();
  return true;
}

void deinitMic() {
  if (retirementPending()) return;
  if (!lockI2s(pdMS_TO_TICKS(500))) {
    Serial.println("[hal_audio] deinitMic: lock timeout");
    return;
  }
  if (s_micInstalled) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    s_micInstalled = false;
    Serial.println("[hal_audio] Mic deinitialized");
  }
  unlockI2s();
}

bool micReady() {
  return !retirementPending() && s_micInstalled;
}

esp_err_t readMic(void* data, size_t bytes, size_t* bytesRead, TickType_t timeout) {
  if (retirementPending() || !s_micInstalled) {
    *bytesRead = 0;
    return ESP_ERR_INVALID_STATE;
  }
  return i2s_read(I2S_NUM_0, data, bytes, bytesRead, timeout);
}

// -----------------------------------------------------------------------------
// I2S TX (DAC playback)
// -----------------------------------------------------------------------------
static void logI2sHeap(const char* phase) {
  Serial.printf("[hal_audio] %s heap: intFree=%u intLargest=%u dmaLargest=%u\n",
                phase,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

bool txBegin(uint32_t sampleRate) {
  if (retirementPending()) return false;
  if (!lockI2s(pdMS_TO_TICKS(500))) {
    Serial.println("[hal_audio] txBegin: lock timeout");
    return false;
  }

  // Ensure RX/TX are not installed from any prior mode.
  if (s_micInstalled || s_txInstalled) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    s_micInstalled = false;
    s_txInstalled = false;
  }

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = (int)sampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = true;
  cfg.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_WS_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num = -1;

  struct TxDmaProfile {
    int dmaBufCount;
    int dmaBufLen;
    bool useApll;
  };
  static constexpr TxDmaProfile kHighRateProfiles[] = {
      {8, 256, true},
      {4, 192, true},
      {4, 128, false},
  };
  static constexpr TxDmaProfile kLowRateProfiles[] = {
      // Synth SFX are fed by the UI task. Give them 23-46 ms of queued audio
      // so a display flush cannot drain I2S between generator calls.
      {4, 256, false},
      {4, 192, false},
      {4, 128, false},
      {2, 128, false},
  };

  const TxDmaProfile* profiles = kHighRateProfiles;
  size_t profileCount = sizeof(kHighRateProfiles) / sizeof(kHighRateProfiles[0]);
  if (sampleRate <= 24000u) {
    profiles = kLowRateProfiles;
    profileCount = sizeof(kLowRateProfiles) / sizeof(kLowRateProfiles[0]);
  }

  esp_err_t lastErr = ESP_OK;
  for (size_t i = 0; i < profileCount; ++i) {
    cfg.dma_buf_count = profiles[i].dmaBufCount;
    cfg.dma_buf_len = profiles[i].dmaBufLen;
    cfg.use_apll = profiles[i].useApll;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    if (err != ESP_OK) {
      lastErr = err;
      Serial.printf("[hal_audio] I2S TX install failed: err=%d sr=%u dma=%dx%d apll=%d\n",
                    (int)err, (unsigned)sampleRate, cfg.dma_buf_count, cfg.dma_buf_len,
                    cfg.use_apll ? 1 : 0);
      logI2sHeap("txBegin");
      continue;
    }

    err = i2s_set_pin(I2S_NUM_0, &pins);
    if (err != ESP_OK) {
      lastErr = err;
      Serial.printf("[hal_audio] I2S TX pin config failed: err=%d\n", (int)err);
      i2s_driver_uninstall(I2S_NUM_0);
      continue;
    }

    err = i2s_start(I2S_NUM_0);
    if (err != ESP_OK) {
      lastErr = err;
      Serial.printf("[hal_audio] I2S TX start failed: err=%d\n", (int)err);
      i2s_driver_uninstall(I2S_NUM_0);
      continue;
    }

    if (i > 0) {
      Serial.printf("[hal_audio] I2S TX using fallback dma=%dx%d apll=%d\n",
                    cfg.dma_buf_count, cfg.dma_buf_len, cfg.use_apll ? 1 : 0);
    }
    s_txInstalled = true;
    s_txDmaBytes = (size_t)cfg.dma_buf_len * 2u * sizeof(int16_t);
    s_txDmaOffset = 0;
    ++s_txGeneration;
    unlockI2s();
    return true;
  }

  Serial.printf("[hal_audio] I2S TX unavailable after fallbacks: lastErr=%d\n", (int)lastErr);
  unlockI2s();
  return false;
}

uint32_t txGeneration() { return s_txGeneration; }

esp_err_t txWrite(const int16_t* samples, size_t sampleCount, size_t* bytesWritten, TickType_t timeout) {
  if (retirementPending() || !s_txInstalled) {
    if (bytesWritten) *bytesWritten = 0;
    return ESP_ERR_INVALID_STATE;
  }
  size_t accepted = 0;
  const esp_err_t err =
      i2s_write(I2S_NUM_0, samples, sampleCount * sizeof(int16_t), &accepted, timeout);
  if (bytesWritten) *bytesWritten = accepted;
  if (s_txDmaBytes) s_txDmaOffset = (s_txDmaOffset + accepted) % s_txDmaBytes;
  return err;
}

bool txFinishBuffer(TickType_t timeout) {
  if (retirementPending()) return false;
  if (!s_txInstalled || !s_txDmaBytes) return false;
  if (!s_txDmaOffset) return true;
  // Every TX profile uses at most 256 stereo frames per DMA buffer.
  static const int16_t silence[256 * 2] = {};
  const size_t remaining = s_txDmaBytes - s_txDmaOffset;
  if (remaining > sizeof(silence) || (remaining % sizeof(int16_t)) != 0) return false;
  size_t accepted = 0;
  const esp_err_t err =
      txWrite(silence, remaining / sizeof(int16_t), &accepted, timeout);
  return err == ESP_OK && accepted == remaining && s_txDmaOffset == 0;
}

void txEnd() {
  if (retirementPending()) return;
  if (!lockI2s(pdMS_TO_TICKS(500))) {
    Serial.println("[hal_audio] txEnd: lock timeout");
    return;
  }
  if (s_txInstalled) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    s_txInstalled = false;
  }
  s_txDmaBytes = 0;
  s_txDmaOffset = 0;
  unlockI2s();
}

bool txReady() {
  return !retirementPending() && s_txInstalled;
}

}  // namespace audio
}  // namespace hal
