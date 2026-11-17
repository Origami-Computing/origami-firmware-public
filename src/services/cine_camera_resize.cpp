#include "services/cine_camera.h"
#include "core/watchdog.h"
#include "core/cine_frame.h"
#include <esp_heap_caps.h>
#include <JPEGDEC.h>
#include <esp_jpeg_dec.h>
#include <new>
#include <cstring>
namespace services {
namespace cine_camera {
namespace {
struct Decode {
  explicit Decode(uint16_t *pixels) : preview(pixels) {}
  uint16_t *preview = nullptr;
  unsigned width = 256, height = 192;
  bool valid = false, interrupted = false;
  bool (*shouldAbort)(void *) = nullptr;
  void *context = nullptr;
  bool abort() {
    if (shouldAbort && shouldAbort(context))
      interrupted = true;
    return interrupted;
  }
};
// Same reduced-IDCT decoder as the stock camera; direct RGB565 callback.
int writeBlock565(JPEGDRAW *block) {
  if (!block || !block->pUser || !block->pPixels)
    return 0;
  auto &c = *static_cast<Decode *>(block->pUser);
  if (c.abort())
    return 0;
  core::watchdog::feed();
  const int x = block->x, y = block->y, w = block->iWidthUsed, h = block->iHeight;
  if (!c.preview || !c.width || !c.height || x < 0 || y < 0 || w <= 0 || h <= 0 ||
      block->iWidth < w || unsigned(block->iWidth) > c.width || unsigned(x) > c.width ||
      unsigned(y) > c.height || unsigned(w) > c.width - unsigned(x) ||
      unsigned(h) > c.height - unsigned(y) ||
      size_t(block->iWidth) * unsigned(h) > MAX_BUFFERED_PIXELS) {
    c.valid = false;
    return 0;
  }
  constexpr unsigned dw = Session::previewWidth, dh = Session::previewHeight;
  if (dw == c.width && dh == c.height) {
    for (int row = 0; row < h; ++row)
      memcpy(c.preview + (y + row) * dw + x, block->pPixels + row * block->iWidth,
             size_t(w) * sizeof(uint16_t));
    return 1;
  }
  const unsigned cropWidth = c.height * dw / dh;
  const unsigned left = (c.width - cropWidth) / 2;
  const unsigned from = unsigned(x) > left ? unsigned(x) - left : 0;
  const unsigned to = unsigned(x + w) > left ? unsigned(x + w) - left : 0;
  const unsigned x0 = (from * dw + cropWidth - 1) / cropWidth,
                 x1 = (to * dw + cropWidth - 1) / cropWidth;
  const unsigned y0 = (unsigned(y) * dh + c.height - 1) / c.height,
                 y1 = (unsigned(y + h) * dh + c.height - 1) / c.height;
  for (unsigned dy = y0; dy < y1 && dy < dh; ++dy) {
    auto* dst = c.preview + dy * dw;
    const auto* src = block->pPixels + (dy * c.height / dh - y) * block->iWidth;
    for (unsigned dx = x0; dx < x1 && dx < dw; ++dx)
      dst[dx] = src[left + dx * cropWidth / dw - x];
  }
  return 1;
}
} // namespace
void Session::releaseIspPreview() {
  if (videoPreviewDecoder_) jpeg_dec_close(videoPreviewDecoder_);
  videoPreviewDecoder_ = nullptr;
  triedVideoDecoder_ = false;
  if (ispDecoder_) {
    ispDecoder_->close();
    ispDecoder_->~JPEGDEC();
    heap_caps_free(ispDecoder_);
    ispDecoder_ = nullptr;
  }
  heap_caps_free(previewWork_);
  previewWork_ = nullptr;
}
bool Session::renderIspPreview(const hal::camera::CameraFrame &frame, bool (*shouldAbort)(void *),
                               void *context) {
  const bool still = frame.width == 2048 && frame.height == 1536;
  const bool hd = frame.width == 1280 && frame.height == 720;
  const bool vga = frame.width == 640 && frame.height == 480;
  const bool qvga = frame.width == 320 && frame.height == 240;
  if (!preview_ || !(still || hd || vga || qvga) ||
      !core::cine::validJpeg(frame.buf, frame.len, frame.width, frame.height, frame.width, frame.height))
    return false;
  if (!previewWork_)
    previewWork_ = static_cast<uint16_t *>(
        heap_caps_aligned_alloc(16, previewWidth * previewHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!previewWork_)
    return false;
  // Scale the live sensor JPEG or recording mailbox frame directly into the
  // inactive viewfinder surface. Only the widescreen feed needs a center crop.
  if (qvga || (!still && !shouldAbort)) {
    if (!triedVideoDecoder_) {
      triedVideoDecoder_ = true;
      jpeg_dec_config_t config{};
      config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
      config.scale.width = hd ? 344 : previewWidth;
      config.scale.height = previewHeight;
      config.clipper.width = previewWidth;
      config.clipper.height = previewHeight;
      if (jpeg_dec_open(&config, &videoPreviewDecoder_) != JPEG_ERR_OK)
        videoPreviewDecoder_ = nullptr;
    }
    if (videoPreviewDecoder_) {
      jpeg_dec_io_t io{};
      jpeg_dec_header_info_t header{};
      io.inbuf = const_cast<uint8_t*>(frame.buf);
      io.inbuf_len = int(frame.len);
      io.outbuf = reinterpret_cast<uint8_t*>(previewWork_);
      int bytes = 0;
      if (jpeg_dec_parse_header(videoPreviewDecoder_, &io, &header) == JPEG_ERR_OK &&
          header.width == previewWidth && header.height == previewHeight &&
          jpeg_dec_get_outbuf_len(videoPreviewDecoder_, &bytes) == JPEG_ERR_OK &&
          bytes == int(previewWidth * previewHeight * sizeof(uint16_t)) &&
          jpeg_dec_process(videoPreviewDecoder_, &io) == JPEG_ERR_OK && io.out_size == bytes) {
        auto* previous = preview_; preview_ = previewWork_; previewWork_ = previous;
        return true;
      }
    }
  }
  Decode c{previewWork_};
  c.shouldAbort = shouldAbort;
  c.context = context;
  bool ok = false;
  if (!ispDecoder_) {
    void *memory = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory)
      ispDecoder_ = new (memory) JPEGDEC;
  }
  if (!ispDecoder_)
    return false;
  ok = ispDecoder_->openRAM(const_cast<uint8_t *>(frame.buf), int(frame.len), writeBlock565) != 0;
  if (ok)
    ok = ispDecoder_->getWidth() == frame.width && ispDecoder_->getHeight() == frame.height;
  if (ok) {
    ispDecoder_->setUserPointer(&c);
    ispDecoder_->setPixelType(RGB565_LITTLE_ENDIAN);
    c.valid = true;
    const unsigned divisor = still ? 8 : hd ? 4 : qvga ? 1 : 2;
    c.width = frame.width / divisor;
    c.height = frame.height / divisor;
    ok = ispDecoder_->decode(0, 0, still ? JPEG_SCALE_EIGHTH : hd ? JPEG_SCALE_QUARTER : qvga ? 0 : JPEG_SCALE_HALF) != 0 && c.valid;
  }
  ispDecoder_->close();
  status_.previewInterrupted = c.interrupted;
  // JPEGDEC treats a draw-callback stop as success; it is not a complete image.
  ok = ok && !c.interrupted;
  // Never present a partially decoded frame, even if another UI action redraws.
  if (ok) {
    auto *completed = preview_;
    preview_ = previewWork_;
    previewWork_ = completed;
  }
  return ok;
}
} // namespace cine_camera
} // namespace services
