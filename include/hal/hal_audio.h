#pragma once

#include <Adafruit_TLV320DAC3100.h>
#include <driver/i2s.h>

// Audio HAL - manages TLV320DAC3100 codec via I2C and I2S for mic input
// This HAL is the single owner of ESP32 I2S driver state.
// App code must not call any i2s_* APIs directly.
// Pending worker retirement temporarily rejects codec/driver mutations and new
// reads/writes; readiness queries report unavailable. Existing in-flight calls can finish.
// Saved output-route preferences remain writable and apply when playback restores.

namespace hal {
namespace audio {

// Codec initialization
void hardwareReset();
bool initCodec(uint8_t i2cAddress = 0x18);
/** True after a successful initCodec() (or after configureForPlayback succeeded post-reset). */
bool codecReady();

// Codec configuration  
bool configureForPlayback();     // Configure codec for DAC output
bool configureForRecording();    // Configure codec for ADC input (mic)

/** Enable the TLV320 jack detector and route its event to codec GPIO1/INT1. */
bool configureHeadsetDetection();
/** Raw TLV320 headset status: 0=none, 1=headphones, 3=headset with microphone. */
uint8_t headsetStatus();
/** Read codec IRQ flags; clearSticky=true clears latched flags after reading. */
uint8_t headsetIrqFlags(bool clearSticky = true);
/** Diagnostic raw codec register read. */
bool readRegister(uint8_t page, uint8_t reg, uint8_t* value);

/** Monotonic counter bumped on every configureForPlayback/Recording(). */
uint32_t codecConfigGeneration();

/**
 * End of a mic capture session: uninstall I2S RX and reconfigure the TLV320 for
 * speaker/headphone playback (DAC path). Call after every finished mic use.
 */
void endMicSession();

/** Full UI playback restore: endMicSession() + tear down any I2S TX + SFX cache reset. */
void restoreForUiPlayback();

/** Invalidate all cached codec/I2S state after waking from light sleep. The codec
 *  PLL is derived from I2S BCLK, which stops during sleep, so the codec config is
 *  dead on wake even though s_codecInitialized survived in RAM. Forces the next
 *  playback to fully re-init the codec (hardwareReset + initCodec + reconfigure),
 *  so the wake chime / alarm actually produce sound. */
void invalidateForWake();
void setSpeakerEnabled(bool on); // Force speaker amp on/off
void setHeadphoneEnabled(bool on); // Force HPL/HPR driver on/off (independent of speaker)

/** Selected output mode. Numeric values are persisted in NVS; do not reorder. */
enum class OutputRoute {
  Both = 0,           ///< Speaker amp + HP driver both on (default).
  SpeakerOnly = 1,    ///< Speaker on, HP driver powered down.
  HeadphonesOnly = 2, ///< HP driver on, speaker amp off.
  Auto = 3,          ///< Speaker unless a headphone/headset plug is detected.
};
void setOutputRoute(OutputRoute r);
OutputRoute getOutputRoute();
/** Resolved physical route (never Auto); also selects the media volume profile. */
OutputRoute getEffectiveOutputRoute();
/** Poll jack detection on the UI task (the codec I2C owner). */
void serviceOutputRoute(uint32_t nowMs);

/** Persist route and apply immediately. */
void outputRouteSaveToNvs(OutputRoute r);

// -----------------------------------------------------------------------------
// I2S TX (DAC playback)
// -----------------------------------------------------------------------------
// Uses I2S_NUM_0 in TX mode with external I2S pins defined in Pins.h.
// This is intentionally minimal: apps can implement decoding/ring buffers,
// but the HAL owns install/uninstall and write calls.
bool txBegin(uint32_t sampleRate);
esp_err_t txWrite(const int16_t* samples, size_t sampleCount, size_t* bytesWritten, TickType_t timeout);
/** Pad the current DMA buffer with zeros so the next finite sound starts cleanly. */
bool txFinishBuffer(TickType_t timeout);
void txEnd();
bool txReady();
/** Monotonic counter incremented on every successful txBegin(). Use to detect TX ownership changes. */
uint32_t txGeneration();

// Microphone (I2S RX) — I2S_NUM_0 is shared with DAC TX; only one mode at a time.
bool initMic(bool streaming = false);   // Codec → recording, then install I2S RX
/** Uninstall I2S RX only (no codec change). For transient pause inside one recording app. */
void deinitMic();
bool micReady();
esp_err_t readMic(void* data, size_t bytes, size_t* bytesRead, TickType_t timeout);

}  // namespace audio
}  // namespace hal
