#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core { namespace cine {
// Bounded APP1 metadata only. The original JPEG payload is never re-encoded.
constexpr size_t exifCapacity = 512;
struct ExifInfo {
    const char* firmwareVersion;
    const char* colorName;
    uint32_t exposureUs; // Zero omits ExposureTime when telemetry is unavailable.
    uint16_t width, height;
    bool automatic;
};
// Returns the complete APP1 segment length, or zero on invalid input/capacity.
// No lens, ISO, clock, GPS or per-device serial fields are generated.
size_t buildExif(uint8_t* output, size_t capacity, const ExifInfo& info);
// Keep a leading JFIF APP0 first. Zero rejects malformed SOI/EOI or APP0 bounds.
size_t exifInsertionOffset(const uint8_t* jpeg, size_t length);
} }
