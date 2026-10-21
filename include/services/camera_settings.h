#pragma once
#include "hal/hal_camera.h"
#include "services/camera_capture.h"

namespace services { namespace camera {
// Persisted values; these modes select ISP profiles, not UI screens.
enum class Mode : uint8_t { Default=0, Bright, Dark, Manual };
enum class Setting : uint8_t {
  Mode, Autofocus, Capture, Exposure, Gain, WhiteBalance, Saturation, Contrast, Sharpness
};

// UI-task configuration record. The app edits a draft and chooses when to save.
// No driver calls, reservations or worker; Session applies the resulting tuning.
struct Settings {
  Mode mode = Mode::Default;
  bool autofocus = true;
  CaptureResolution capture = CaptureResolution::Vga;
  hal::camera::Tuning manual{};

  // Load persisted fields in place; retain all other manual tuning fields.
  void load();
  // Write only the selected field. Failure does not roll back this record.
  bool save(Setting field) const;
  // Compose the selected profile and AF choice; always continuous AF.
  hal::camera::Tuning tuning() const;
};
} }
