#pragma once
#include <stdint.h>
namespace services { namespace camera {
// Persisted request values. Actual dimensions may be clamped by the sensor;
// FiveMp retains the existing QSXGA request, not a capability guarantee.
enum class CaptureResolution : uint8_t { Vga=0, Hd=1, FiveMp=2 };
const char* captureLabel(CaptureResolution requested);
// Existing largest-contiguous-PSRAM gate, independent of sensor capabilities.
CaptureResolution maxCaptureResolution();
} }
