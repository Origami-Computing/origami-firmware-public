#pragma once

#include <stddef.h>
#include <stdint.h>

namespace services {
namespace camera {
namespace capture_policy {

constexpr size_t kMaxJpegBytes = 256u * 1024u;

inline constexpr bool validVgaJpeg(const uint8_t* data, size_t len, unsigned width, unsigned height) {
  return data && len >= 4 && len <= kMaxJpegBytes && width == 640 && height == 480 &&
         data[0] == 0xff && data[1] == 0xd8 && data[len - 2] == 0xff && data[len - 1] == 0xd9;
}

inline constexpr bool freshFrame(uint64_t dmaStartUs, uint64_t requestUs) {
  return dmaStartUs != 0 && dmaStartUs >= requestUs;
}

} } }
