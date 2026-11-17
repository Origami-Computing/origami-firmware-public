#pragma once
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
namespace core {
namespace cine {
// The driver labels a queued buffer using the current sensor size. Check the
// JPEG itself before saving a frame across a live sensor-size transition.
inline bool hasJpegDimensions(const uint8_t* data, size_t size, unsigned width, unsigned height) {
  if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
  size_t pos = 2;
  while (pos + 4 <= size && data[pos++] == 0xff) {
    while (pos < size && data[pos] == 0xff) ++pos;
    if (size - pos < 3) return false;
    const unsigned marker = data[pos++];
    const size_t length = (unsigned(data[pos]) << 8) | data[pos + 1];
    if (length < 2 || length > size - pos || marker == 0xda) return false;
    if (marker == 0xc0)
      return length >= 8 && ((unsigned(data[pos + 3]) << 8) | data[pos + 4]) == height &&
             ((unsigned(data[pos + 5]) << 8) | data[pos + 6]) == width;
    pos += length;
  }
  return false;
}
inline bool validJpeg(const uint8_t *data, size_t bytes, unsigned width, unsigned height,
                      unsigned expectedWidth, unsigned expectedHeight) {
  return data && expectedWidth && expectedHeight && expectedWidth <= 4096 &&
         expectedHeight <= 4096 && width == expectedWidth && height == expectedHeight &&
         bytes >= 4 && bytes <= size_t(INT_MAX) &&
         bytes <= size_t(expectedWidth) * expectedHeight && data[0] == 0xff && data[1] == 0xd8 &&
         data[bytes - 2] == 0xff && data[bytes - 1] == 0xd9;
}
} // namespace cine
} // namespace core
