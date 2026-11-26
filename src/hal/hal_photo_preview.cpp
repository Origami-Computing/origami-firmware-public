#include "hal/hal_photo_preview.h"
#include "hal/hal_display.h"
#include "hal/hal_sd.h"
#include "core/watchdog.h"
#include <Arduino.h>
#include <SD.h> // JPEGDEC's optional File overload needs the Arduino fs alias.
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <new>
#include <string.h>
namespace hal {
namespace display {
namespace {
constexpr size_t kMaxJpeg = 2u * 1024 * 1024;
void *allocate(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
struct Reader {
  hal::sd::StreamReader stream;
  ~Reader() { hal::sd::closeStream(&stream); }
};
// Maps decoded source blocks onto a dw x dh scaled image, keeping only the
// ow x oh window at (ox, oy): the whole image when fitting, the centre crop
// when covering.
struct Sink {
  uint16_t *pixels;
  unsigned sw, sh, dw, dh, written;
  bool valid;
  unsigned ox, oy, ow, oh;
};
int paint(JPEGDRAW *block) {
  if (!block || !block->pUser)
    return 0;
  auto &s = *static_cast<Sink *>(block->pUser);
  core::watchdog::feed();
  const int x = block->x, y = block->y, w = block->iWidthUsed, h = block->iHeight,
            stride = block->iWidth;
  if (!block->pPixels || !s.pixels || !s.sw || !s.sh || x < 0 || y < 0 || w <= 0 || h <= 0 ||
      stride < w || unsigned(x) > s.sw || unsigned(y) > s.sh || unsigned(w) > s.sw - unsigned(x) ||
      unsigned(h) > s.sh - unsigned(y) || unsigned(stride) > MAX_BUFFERED_PIXELS / unsigned(h)) {
    s.valid = false;
    return 0;
  }
  const unsigned x0 = (unsigned(x) * s.dw + s.sw - 1) / s.sw,
                 x1 = ((unsigned(x) + unsigned(w)) * s.dw + s.sw - 1) / s.sw;
  const unsigned y0 = (unsigned(y) * s.dh + s.sh - 1) / s.sh,
                 y1 = ((unsigned(y) + unsigned(h)) * s.dh + s.sh - 1) / s.sh;
  for (unsigned dy = y0 > s.oy ? y0 : s.oy; dy < y1 && dy < s.dh && dy < s.oy + s.oh; ++dy)
    for (unsigned dx = x0 > s.ox ? x0 : s.ox; dx < x1 && dx < s.dw && dx < s.ox + s.ow; ++dx) {
      s.pixels[(dy - s.oy) * s.ow + (dx - s.ox)] =
          block->pPixels[(dy * s.sh / s.dh - unsigned(y)) * unsigned(stride) +
                         (dx * s.sw / s.dw - unsigned(x))];
      if (++s.written > s.ow * s.oh) {
        s.valid = false;
        return 0;
      }
    }
  return 1;
}
} // namespace
void PhotoPreview::close() {
  if (decoder_) {
    decoder_->close();
    decoder_->~JPEGDEC();
    heap_caps_free(decoder_);
    decoder_ = nullptr;
  }
  heap_caps_free(jpeg_);
  heap_caps_free(pixels_);
  jpeg_ = nullptr;
  pixels_ = nullptr;
  jpegCapacity_ = pixelCapacity_ = 0;
  jpegLoaded_ = false;
}
bool PhotoPreview::draw(const char *path, int x, int y, int maxW, int maxH, Mode mode) {
  timing_ = Timing{};
  if (!path || maxW <= 0 || maxH <= 0 || maxW > 280 || maxH > 240 || x < 0 || y < 0 ||
      x > 280 - maxW || y > 240 - maxH)
    return false;
  const uint32_t started = micros();
  jpegLoaded_ = false;
  auto legacy = [&]() {
    const uint32_t at = micros();
    const bool ok = drawJpegFileScaled(path, x, y, maxW, maxH, cover_ ? ImageFit::Cover : ImageFit::Contain);
    timing_.decodeUs += uint32_t(micros() - at);
    timing_.totalUs = micros() - started;
    return ok;
  };
  if (mode == Mode::Legacy) {
    close();
    return legacy();
  }
  bool loaded = false;
  {
    Reader file;
    uint32_t at = micros();
    const bool opened = hal::sd::openStream(path, &file.stream);
    timing_.openUs = micros() - at;
    if (opened && file.stream.size >= 4 && file.stream.size <= kMaxJpeg) {
      timing_.bytes = file.stream.size;
      at = micros();
      if (jpegCapacity_ < timing_.bytes) {
        heap_caps_free(jpeg_);
        jpeg_ = static_cast<uint8_t *>(allocate(timing_.bytes));
        jpegCapacity_ = jpeg_ ? timing_.bytes : 0;
      }
      timing_.allocUs += uint32_t(micros() - at);
      if (jpeg_) {
        size_t used = 0;
        at = micros();
        while (used < timing_.bytes) {
          core::watchdog::feed();
          size_t n = timing_.bytes - used;
          if (n > 32768)
            n = 32768;
          const size_t got = hal::sd::readStream(&file.stream, jpeg_ + used, n);
          if (!got || got > n)
            break;
          used += got;
        }
        timing_.readUs = micros() - at;
        loaded = used == timing_.bytes;
        jpegLoaded_ = loaded;
      }
    }
  }
  bool ok = false;
  if (loaded) {
    if (mode == Mode::Buffered) {
      const uint32_t at = micros();
      ok = drawJpegBufferScaled(jpeg_, timing_.bytes, x, y, maxW, maxH, cover_ ? ImageFit::Cover : ImageFit::Contain);
      timing_.decodeUs = micros() - at;
    } else
      ok = reduced(jpeg_, x, y, maxW, maxH);
  }
  if (!ok) {
    timing_.fallback = true;
    close();
    return legacy();
  }
  timing_.totalUs = micros() - started;
  return true;
}
bool PhotoPreview::drawBuffer(const uint8_t *data, size_t bytes, int x, int y, int maxW, int maxH) {
  timing_ = Timing{};
  jpegLoaded_ = false;  // jpeg() describes file draws only; this buffer is borrowed
  if (!data || bytes < 4 || bytes > kMaxJpeg || maxW <= 0 || maxH <= 0 || maxW > 280 || maxH > 240 ||
      x < 0 || y < 0 || x > 280 - maxW || y > 240 - maxH)
    return false;
  const uint32_t started = micros();
  timing_.bytes = bytes;
  const bool ok = reduced(data, x, y, maxW, maxH);
  timing_.totalUs = micros() - started;
  return ok;
}
bool PhotoPreview::reduced(const uint8_t *data, int x, int y, int maxW, int maxH) {
  uint32_t at = micros();
  if (!decoder_) {
    void *memory = allocate(sizeof(JPEGDEC));
    if (memory)
      decoder_ = new (memory) JPEGDEC;
  }
  timing_.allocUs += uint32_t(micros() - at);
  if (!decoder_)
    return false;
  at = micros();
  bool ok = decoder_->openRAM(const_cast<uint8_t *>(data), int(timing_.bytes), paint) != 0;
  const int width = ok ? decoder_->getWidth() : 0, height = ok ? decoder_->getHeight() : 0;
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
    decoder_->close();
    return false;
  }
  timing_.width = width;
  timing_.height = height;
  unsigned dw = width, dh = height;
  if (cover_) {
    // Scale so both axes reach the box (up or down), then crop the overflow.
    if (uint64_t(width) * maxH >= uint64_t(height) * maxW) {
      dh = maxH;
      dw = unsigned((uint64_t(width) * maxH + height - 1) / height);
    } else {
      dw = maxW;
      dh = unsigned((uint64_t(height) * maxW + width - 1) / width);
    }
  } else if (dw > unsigned(maxW) || dh > unsigned(maxH)) {
    if (uint64_t(dw) * maxH > uint64_t(dh) * maxW) {
      dh = uint64_t(dh) * maxW / dw;
      dw = maxW;
    } else {
      dw = uint64_t(dw) * maxH / dh;
      dh = maxH;
    }
  }
  if (!dw || !dh) {
    decoder_->close();
    return false;
  }
  unsigned scale = 1;
  int options = 0;
  for (unsigned trial = 2; trial <= 8; trial *= 2)
    if (unsigned(width) / trial >= dw && unsigned(height) / trial >= dh) {
      scale = trial;
      options = trial == 8 ? JPEG_SCALE_EIGHTH : trial == 4 ? JPEG_SCALE_QUARTER : JPEG_SCALE_HALF;
    }
  timing_.scale = scale;
  const unsigned ow = cover_ ? unsigned(maxW) : dw, oh = cover_ ? unsigned(maxH) : dh;
  const unsigned ox = (dw - ow) / 2, oy = (dh - oh) / 2;
  const size_t bytes = size_t(ow) * oh * 2;
  const uint32_t allocateAt = micros();
  if (pixelCapacity_ < bytes) {
    heap_caps_free(pixels_);
    pixels_ = static_cast<uint16_t *>(allocate(bytes));
    pixelCapacity_ = pixels_ ? bytes : 0;
  }
  const uint32_t pixelAllocUs = micros() - allocateAt;
  timing_.allocUs += pixelAllocUs;
  if (!pixels_) {
    decoder_->close();
    return false;
  }
  Sink sink{pixels_,
            (unsigned(width) + scale - 1) / scale,
            (unsigned(height) + scale - 1) / scale,
            dw,
            dh,
            0,
            true,
            ox,
            oy,
            ow,
            oh};
  decoder_->setUserPointer(&sink);
  decoder_->setPixelType(RGB565_LITTLE_ENDIAN);
  ok = decoder_->decode(0, 0, options) != 0 && sink.valid && sink.written == ow * oh;
  decoder_->close();
  timing_.decodeUs = uint32_t(micros() - at) - pixelAllocUs;
  if (!ok)
    return false;
  at = micros();
  auto &output = tft();
  const bool swap = output.getSwapBytes();
  output.setSwapBytes(true);
  output.pushImage(x + (maxW - int(ow)) / 2, y + (maxH - int(oh)) / 2, ow, oh, pixels_);
  output.setSwapBytes(swap);
  timing_.composeUs = micros() - at;
  return true;
}
} // namespace display
} // namespace hal
