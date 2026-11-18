#include "services/cine_camera.h"
#include "services/video_recording.h"
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_timer.h>
#include "core/cine_frame.h"

namespace services { namespace cine_camera {
bool Session::beginClip(camera::VideoRecording &recorder, bool hd, bool microphone) {
  const uint32_t started = millis();
  const auto selected = captureSettings();
  releaseIspPreview(); // Recreate the scaled decoder for the selected clip aspect ratio.
  camera_.returnFrame();
  const bool warm = camera_.setVideoPreviewSize(hd);
  const uint32_t stopped = millis();
  if (!warm) {
    camera_.stop();
    hal::camera::Tuning tuning;
    tuning.allowNightMode = false;
    tuning.wb_mode = 1;
    tuning.lenc = false;
    if (camera_.running() || !camera_.start(hd ? FRAMESIZE_HD : FRAMESIZE_VGA,
                                            PIXFORMAT_JPEG, &tuning, hd ? 16 : 12))
      return false;
  }
  const uint32_t configured = millis();
  if (!warm) camera_.settle(700);
  auto readFresh = [&](uint64_t readyUs, uint64_t& lastFrame) {
    hal::camera::CameraFrame frame{};
    if (!camera_.read(&frame)) return false;
    const bool valid = frame.dmaStartUs >= readyUs && frame.dmaStartUs > lastFrame &&
        frame.format == PIXFORMAT_JPEG &&
        core::cine::validJpeg(frame.buf, frame.len, frame.width, frame.height,
                             hd ? 1280 : 640, hd ? 720 : 480) &&
        core::cine::hasJpegDimensions(frame.buf, frame.len, hd ? 1280 : 640, hd ? 720 : 480);
    if (frame.dmaStartUs > lastFrame) lastFrame = frame.dmaStartUs;
    camera_.returnFrame();
    return valid;
  };
  if (warm) {
    // Let the resolution transition settle before the ISP transaction, as
    // in the cold path. Observe live frames instead of a power-on delay.
    // Auto can finish early only after three <=3% exposure*gain transitions.
    // Unstable light retains a bounded 1.1s wait, as in open().
    const uint64_t switchedAt = esp_timer_get_time();
    const uint64_t deadline = switchedAt + 1100000;
    uint64_t lastFrame = 0, previousExposure = 0;
    unsigned stable = 0;
    bool fresh = false;
    do {
      fresh = false;
      const bool valid = readFresh(switchedAt, lastFrame);
      if (!valid) continue;
      fresh = true;
      if (!selected.autoExposure) break;
      hal::camera::ShutterInfo value{};
      if (!hal::camera::readJpegShutter(&value) || !value.valid) {
        previousExposure = 0;
        stable = 0;
        continue;
      }
      const uint64_t exposure = uint64_t(value.actualUs) * value.gainQ4;
      const uint64_t difference = exposure > previousExposure
          ? exposure - previousExposure : previousExposure - exposure;
      stable = previousExposure && difference * 100 <= previousExposure * 3 ? stable + 1 : 0;
      previousExposure = exposure;
      if (stable >= 3) break;
    } while (uint64_t(esp_timer_get_time()) < deadline);
    Serial.printf("[video-settle] fresh=%u stable=%u elapsed_ms=%lu\n", fresh, stable,
                  (unsigned long)(millis() - configured));
    if (!fresh) return false; // Never hand stale/wrong-size buffers to the writer.
  }
  const uint32_t settled = millis();
  autoFrameVts_ = 0; // Each sensor resolution has its own automatic frame length.
  if (!restoreSettings(selected)) return false;
  const uint32_t restored = millis();
  if (warm) {
    // Do not let the recorder consume a queued pre-ISP frame, including silent
    // mode where microphone setup does not provide an incidental settling gap.
    const uint64_t deadline = shutterReadyUs_ + uint64_t(shutter_.frameUs) * 3 + 250000;
    uint64_t lastFrame = 0;
    bool fresh = false;
    do { fresh = readFresh(shutterReadyUs_, lastFrame); }
    while (!fresh && uint64_t(esp_timer_get_time()) < deadline);
    if (!fresh) return false;
  }
  const uint32_t ready = millis();
  Serial.printf("[camera-clip] color=%u manual=%u exposure_us=%lu\n", selected.colorMode,
                shutter_.manual, (unsigned long)shutter_.actualUs);
  const bool ok = recorder.beginCamera(camera_, hd ? 1280 : 640, hd ? 720 : 480, microphone);
  Serial.printf("[video-prepare] warm=%u stop_ms=%lu configure_ms=%lu settle_ms=%lu isp_ms=%lu recorder_ms=%lu ok=%u\n",
                warm, (unsigned long)(stopped-started), (unsigned long)(configured-stopped),
                (unsigned long)(settled-configured+ready-restored), (unsigned long)(restored-settled),
                (unsigned long)(millis()-ready), ok);
  return ok;
}
bool Session::previewClip(camera::VideoRecording &recorder) {
  hal::camera::CameraFrame frame{};
  if (!recorder.readPreview(&frame)) return false;
  const bool rendered = renderIspPreview(frame, nullptr, nullptr);
  recorder.releasePreview();
  return rendered;
}
} }
