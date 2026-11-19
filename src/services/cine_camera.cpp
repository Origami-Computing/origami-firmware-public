#include "services/cine_camera.h"
#include "services/photo_storage.h"
#include "hal/hal_sd.h"
#include "core/watchdog.h"
#include "core/cine_shutter.h"
#include "core/cine_frame.h"
#include "core/cine_isp.h"
#include "core/cine_exif.h"
#include "core/device_id.h"
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdio.h>
namespace services {
namespace cine_camera {
bool Session::open() {
  if (!close(false))
    return false;
  autoFrameVts_ = 0;
  configured_ = false;
  ispReadback_ = {};
  status_ = {};
  shutter_ = {};
  shutterReadyUs_ = 0;
  colorMode_ = ColorMode::Sunny;
  if (!photos::initialize() || !hal::sd::ensureDir("/cine")) {
    snprintf(status_.message, sizeof(status_.message), "SD unavailable");
    close();
    return false;
  }
  if (camera_.prepareCapture(false) != camera::Session::PrepareResult::Ready) {
    snprintf(status_.message, sizeof(status_.message), "Camera resources busy");
    close();
    return false;
  }
  hal::camera::Tuning tuning;
  tuning.wb_mode = 1;
  tuning.ae_level = core::cine::aeLevel;
  tuning.continuousStillPreview = true;
  tuning.allowNightMode = false;
  tuning.lenc = false;
  if (!camera_.start(FRAMESIZE_QXGA, PIXFORMAT_JPEG, &tuning, core::cine::jpegQuantizationScale)) {
    snprintf(status_.message, sizeof(status_.message), "Camera init failed: %s",
             camera_.lastError());
    close();
    return false;
  }
  preview_ = static_cast<uint16_t *>(
      heap_caps_aligned_alloc(16, previewWidth * previewHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!preview_) {
    snprintf(status_.message, sizeof(status_.message), "Image buffer allocation failed");
    close();
    return false;
  }
  if (!camera_.setStillPreviewSize(false)) {
    close();
    snprintf(status_.message, sizeof(status_.message), "Preview size unavailable");
    return false;
  }
  const uint64_t aeAt = esp_timer_get_time();
  // Observe AE over distinct frames rather than always sleeping 1.1 seconds.
  // Require three stable transitions and >=450ms; retain the old maximum.
  hal::camera::ShutterInfo previous{};
  unsigned stable = 0;
  while (esp_timer_get_time() - aeAt < 1100000) {
    hal::camera::CameraFrame frame{};
    if (!camera_.read(&frame))
      break;
    camera_.returnFrame();
    hal::camera::ShutterInfo value{};
    if (hal::camera::readJpegShutter(&value) && value.valid) {
      const uint64_t before = uint64_t(previous.actualUs) * previous.gainQ4,
                     now = uint64_t(value.actualUs) * value.gainQ4;
      const uint64_t difference = before > now ? before - now : now - before;
      stable = before && difference * 100 <= before * 3 ? stable + 1 : 0;
      previous = value;
      if (stable >= 3 && esp_timer_get_time() - aeAt >= 450000)
        break;
    } else
      stable = 0;
  }

  status_.open = true;
  if (!configureExposure(ExposureMode::Automatic, 16667, 16))
    return false;
  // Keep the two-frame freshness guard after ISP writes. Drain transitional
  // frames until an actual fresh preview is decoded, with a bounded deadline.
  bool first = false;
  do {
    first = preview();
  } while (!first && shutterReadyUs_ &&
           uint64_t(esp_timer_get_time()) <
               shutterReadyUs_ + uint64_t(shutter_.frameUs) * 3 + 250000);
  if (!first) {
    snprintf(status_.message, sizeof(status_.message), "Initial camera frame unavailable");
    close();
    return false;
  }

  return true;
}
bool Session::close(bool restoreUi) {
  status_.open = false;
  camera_.returnFrame();
  camera_.stop();
  // Failed teardown retains ownership and allocations until a safe retry.
  if (camera_.running()) {
    snprintf(status_.message, sizeof(status_.message), "Camera cleanup pending; retry");
    return false;
  }
  releaseIspPreview();
  heap_caps_free(preview_);
  preview_ = nullptr;
  if (restoreUi)
    camera_.restoreUi(camera::Session::Resume::DeferredWithInputRecovery);
  return true;
}
bool Session::preview(bool (*shouldAbort)(void *), void *context) {
  if (!status_.open)
    return false;
  status_.previewInterrupted = false;
  if (shouldAbort && shouldAbort(context)) {
    status_.previewInterrupted = true;
    return false;
  }
  hal::camera::CameraFrame frame{};
  if (!camera_.read(&frame))
    return false;
  if (shouldAbort && shouldAbort(context)) {
    camera_.returnFrame();
    status_.previewInterrupted = true;
    return false;
  }
  if (frame.dmaStartUs < shutterReadyUs_) {
    camera_.returnFrame();
    return false;
  }
  bool ok = frame.format == PIXFORMAT_JPEG &&
            core::cine::validJpeg(frame.buf, frame.len, frame.width, frame.height, 320, 240);
  if (ok)
    ok = renderIspPreview(frame, shouldAbort, context);
  camera_.returnFrame();
  if (!ok && !status_.previewInterrupted)
    snprintf(status_.message, sizeof(status_.message), "Unexpected sensor frame format");
  return ok;
}
void Session::adjustShutter(int delta) {
  if (!status_.open || !shutter_.valid || !delta)
    return;
  // Sample the live hardware result only when handing control to the wheel.
  // Continuous metering itself runs in the sensor; no per-preview I2C polling.
  const bool automatic = autoExposure();
  if (automatic) {
    if (!refreshExposure())
      return;
  }
  int step = int(core::cine::nearestShutter(automatic ? shutter_.actualUs : shutter_.requestedUs));
  if (!automatic && step == 0 && delta < 0) {
    resumeAutoExposure();
    return;
  }
  int next = delta < -int(core::cine::shutterCount)  ? 0
             : delta > int(core::cine::shutterCount) ? int(core::cine::shutterCount) - 1
                                                     : step + delta;
  if (next < 0)
    next = 0;
  if (next >= int(core::cine::shutterCount))
    next = core::cine::shutterCount - 1;
  if (!automatic && next == step && shutter_.requestedUs == core::cine::shutterSpeedsUs[next])
    return;
  configureExposure(ExposureMode::Manual, core::cine::shutterSpeedsUs[next], shutter_.gainQ4);
}
bool Session::capture(void (*onFrameReady)(), bool enqueueUpload) {
  status_.saved = status_.queued = false;
  if (!status_.open)
    return false;
  const uint64_t started = esp_timer_get_time();
  if (!shutter_.valid || started < shutterReadyUs_) {
    snprintf(status_.message, sizeof(status_.message),
             shutter_.valid ? "Updating shutter; wait for preview"
                            : "Reopen camera for shutter control");
    return false;
  }
  const auto settings = captureSettings();
  if (!switchStillSize(true, settings))
    return false;
  const bool saved = saveStill(onFrameReady, enqueueUpload);
  if (!switchStillSize(false, settings))
    return false;
  status_.saved = saved;
  return saved;
}

bool Session::switchStillSize(bool fullResolution, const hal::camera::CineIspRequest& settings) {
  autoFrameVts_ = 0;
  if (camera_.setStillPreviewSize(fullResolution) && restoreSettings(settings))
    return true;
  close();
  snprintf(status_.message, sizeof(status_.message), "Camera size change failed");
  return false;
}

bool Session::saveStill(void (*onFrameReady)(), bool enqueueUpload) {
  const uint64_t started = esp_timer_get_time();
  status_.saved = false;
  status_.photoPath[0] = status_.metadataPath[0] = 0;
  auto target = photos::prepare();
  if (!target.valid()) {
    snprintf(status_.message, sizeof(status_.message), "Could not create photo");
    return false;
  }
  status_.sequence = target.sequence;
  hal::camera::CameraFrame frame{};
  bool acquired = false;
  for (unsigned i = 0; i < 12; ++i) {
    core::watchdog::feed();
    if (!camera_.read(&frame))
      break;
    if (frame.dmaStartUs + 25000 >= started && frame.dmaStartUs >= shutterReadyUs_ &&
        core::cine::hasJpegDimensions(frame.buf, frame.len, 2048, 1536) &&
        frame.format == PIXFORMAT_JPEG &&
        core::cine::validJpeg(frame.buf, frame.len, frame.width, frame.height, 2048, 1536)) {
      acquired = true;
      break;
    }
    camera_.returnFrame();
  }
  if (!acquired) {
    snprintf(status_.message, sizeof(status_.message), "Fresh sensor frame unavailable");
    return false;
  }
  photos::markCaptured(target);
  status_.capturedAtUnix = target.capturedAtUnix;
  const bool sampleValid = autoExposure() ? refreshExposure() : shutter_.valid;
  const auto exposure = sampleValid ? shutter_ : hal::camera::ShutterInfo{};
  if (onFrameReady) onFrameReady();
  uint8_t exif[core::cine::exifCapacity];
  const core::cine::ExifInfo exifInfo{core::device::firmwareVersion(), colorName(colorMode_),
      exposure.valid ? exposure.actualUs : 0, uint16_t(frame.width), uint16_t(frame.height),
      autoExposure(), target.capturedAtUnix};
  const size_t exifBytes = core::cine::buildExif(exif, sizeof(exif), exifInfo);
  const bool ok =
      frame.format == PIXFORMAT_JPEG &&
      core::cine::validJpeg(frame.buf, frame.len, frame.width, frame.height, 2048, 1536) &&
      photos::write(target, frame.buf, frame.len, exif, exifBytes);
  camera_.returnFrame();
  status_.saved = ok;
  // Persist the upload request only after the JPEG is saved and the sensor
  // frame is returned. The camera reservation defers network work until exit.
  status_.queued = ok && enqueueUpload && photos::enqueue(target);
  snprintf(status_.photoPath, sizeof(status_.photoPath), "%s", target.path);
  snprintf(status_.metadataPath, sizeof(status_.metadataPath), "/cine/SHOT_%05lu.json",
           (unsigned long)status_.sequence);
  char metadata[1200];
  const int n = snprintf(metadata, sizeof(metadata),
                         "{\"version\":3,\"mode\":\"Cine ISP 3MP\",\"source\":\"sensor ISP "
                         "JPEG\",\"output_width\":2048,\"output_height\":1536,"
                         "\"jpeg_quantization_scale\":%u,\"embedded_exif\":%s,"
                         "\"color_profile\":\"sensor-ISP\",\"color_version\":2,"
                         "\"color_mode\":%u,\"color_name\":\"%s\",\"gamma\":\"%s\",\"wb_preset\":1,"
                         "\"shutter\":{\"sample_valid\":%s,\"sample_source\":\"%s\","
                         "\"automatic\":%s,\"ae_level\":%s,\"manual\":%s,"
                         "\"requested_us\":%lu,\"actual_us\":%lu,\"integration_ticks_q4\":%lu,"
                         "\"gain_q4\":%u,\"hts\":%u,\"vts\":%u,\"clock_hz\":%lu},\"saved\":%s}",
                         unsigned(core::cine::jpegQuantizationScale), ok && exifBytes ? "true" : "false", unsigned(colorMode_),
                         colorName(colorMode_), colorMode_ == ColorMode::Mono
                             ? "sRGB + classic silver" : "sRGB approximation",
                         sampleValid ? "true" : "false",
                         autoExposure() ? "live-after-frame"
                         : sampleValid  ? "locked"
                                        : "unavailable",
                         autoExposure() ? "true" : "false", autoExposure() ? "-1" : "null",
                         exposure.valid && exposure.manual ? "true" : "false",
                         (unsigned long)exposure.requestedUs, (unsigned long)exposure.actualUs,
                         (unsigned long)exposure.ticks, exposure.gainQ4, exposure.hts, exposure.vts,
                         (unsigned long)exposure.clockHz, ok ? "true" : "false");
  const bool metadataOk =
      n > 0 && size_t(n) < sizeof(metadata) &&
      hal::sd::writeFile(status_.metadataPath, reinterpret_cast<const uint8_t *>(metadata), n);
  snprintf(status_.message, sizeof(status_.message),
           ok ? (enqueueUpload && !status_.queued ? "Photo saved; not queued. Retry in Photos"
                 : metadataOk && exifBytes ? "Photo saved" : "Photo saved; metadata failed")
              : "Save failed; partial files retained");
  return ok;
}
} // namespace cine_camera
} // namespace services
