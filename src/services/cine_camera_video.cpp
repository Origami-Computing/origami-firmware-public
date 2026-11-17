#include "services/cine_camera.h"
#include "services/video_recording.h"
#include <Arduino.h>
#include <esp_camera.h>

namespace services { namespace cine_camera {
bool Session::beginClip(camera::VideoRecording &recorder, bool hd, bool microphone) {
  const auto selected = captureSettings();
  releaseIspPreview(); // Recreate the scaled decoder for the selected clip aspect ratio.
  camera_.returnFrame();
  camera_.stop();
  hal::camera::Tuning tuning;
  tuning.allowNightMode = false;
  tuning.wb_mode = 1;
  tuning.lenc = false;
  if (camera_.running() || !camera_.start(hd ? FRAMESIZE_HD : FRAMESIZE_VGA,
                                          PIXFORMAT_JPEG, &tuning, hd ? 16 : 12))
    return false;
  camera_.settle(700);
  autoFrameVts_ = 0; // Each sensor resolution has its own automatic frame length.
  if (!restoreSettings(selected)) return false;
  Serial.printf("[camera-clip] color=%u manual=%u exposure_us=%lu\n", selected.colorMode,
                shutter_.manual, (unsigned long)shutter_.actualUs);
  return recorder.beginCamera(camera_, hd ? 1280 : 640, hd ? 720 : 480, microphone);
}
bool Session::previewClip(camera::VideoRecording &recorder) {
  hal::camera::CameraFrame frame{};
  if (!recorder.readPreview(&frame)) return false;
  const bool rendered = renderIspPreview(frame, nullptr, nullptr);
  recorder.releasePreview();
  return rendered;
}
} }
