#pragma once
#include "services/camera_session.h"
#include "core/cine_isp.h"
class JPEGDEC;
namespace services {
namespace camera { class VideoRecording; }
namespace cine_camera {
using ColorMode = core::cine::ColorMode;
struct Status {
  bool open = false, saved = false, previewInterrupted = false;
  uint32_t sequence = 0;
  char message[80] = {}, photoPath[64] = {}, metadataPath[64] = {};
};
// UI-owned, single 3MP ISP/JPEG pipeline. All frame loans end before input dispatch.
class Session {
public:
  Session() = default;
  ~Session() { close(); }
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  bool open();
  bool close(bool restoreUi = true);
  bool holdsResources() const { return camera_.backgroundHeld() || camera_.running(); }
  bool preview(bool (*shouldAbort)(void *) = nullptr, void *context = nullptr);
  bool capture();
  bool beginClip(camera::VideoRecording &recorder, bool hd, bool microphone);
  bool previewClip(camera::VideoRecording &recorder);
  void adjustShutter(int delta);
  bool autoExposure() const { return configured_ && exposureMode_ == ExposureMode::Automatic; }
  bool resumeAutoExposure();
  hal::camera::CineIspRequest captureSettings() const;
  bool restoreSettings(const hal::camera::CineIspRequest &settings);
  bool refreshExposure();
  const hal::camera::ShutterInfo &shutter() const { return shutter_; }
  void adjustColor(int delta);
  ColorMode colorMode() const { return colorMode_; }
  static const char *colorName(ColorMode preset);
  const Status &status() const { return status_; }
  const uint16_t *pixels() const { return preview_; }
  static constexpr unsigned previewWidth = 256, previewHeight = 192;
private:
  enum class ExposureMode { Automatic, Manual };
  camera::Session camera_;
  uint16_t *preview_ = nullptr, *previewWork_ = nullptr;
  void* videoPreviewDecoder_ = nullptr;
  bool triedVideoDecoder_ = false;
  JPEGDEC *ispDecoder_ = nullptr;
  ColorMode colorMode_ = ColorMode::Sunny;
  ExposureMode exposureMode_ = ExposureMode::Automatic;
  hal::camera::ShutterInfo shutter_;
  uint64_t shutterReadyUs_ = 0;
  uint16_t autoFrameVts_ = 0;
  bool configured_ = false;
  Status status_;
  hal::camera::CineIspReadback ispReadback_;
  bool configureExposure(ExposureMode mode, uint32_t shutterUs, unsigned gainQ4);
  bool switchStillSize(bool fullResolution, const hal::camera::CineIspRequest& settings);
  bool saveStill();
  void releaseIspPreview();
  bool renderIspPreview(const hal::camera::CameraFrame &frame, bool (*shouldAbort)(void *),
                        void *context);
};
} // namespace cine_camera
} // namespace services
