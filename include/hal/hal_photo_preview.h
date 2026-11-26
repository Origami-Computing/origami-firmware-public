#pragma once
#include <stddef.h>
#include <stdint.h>
class JPEGDEC;
namespace hal {
namespace display {
// UI-owned gallery renderer. Originals are read-only. All workspace is PSRAM;
// release before handing the peripheral reservation to a camera/other app.
class PhotoPreview {
public:
  enum class Mode { Reduced, Buffered, Legacy };
  struct Timing {
    uint32_t openUs = 0, allocUs = 0, readUs = 0, decodeUs = 0, composeUs = 0, totalUs = 0;
    size_t bytes = 0;
    unsigned width = 0, height = 0, scale = 1;
    bool fallback = false;
  };
  PhotoPreview() = default;
  ~PhotoPreview() { close(); }
  PhotoPreview(const PhotoPreview &) = delete;
  PhotoPreview &operator=(const PhotoPreview &) = delete;
  bool draw(const char *path, int x, int y, int maxW, int maxH, Mode mode = Mode::Reduced);
  // Reduced decode of a JPEG already in memory (e.g. a video's first frame).
  // data is borrowed for this call only.
  bool drawBuffer(const uint8_t *data, size_t bytes, int x, int y, int maxW, int maxH);
  // Cover: fill the whole maxW x maxH box (scaling up if needed) and crop the
  // overflow centrally, instead of fitting inside it with bars.
  void setCover(bool cover) { cover_ = cover; }
  // The last drawn file's bytes (valid until the next draw/close), e.g. for EXIF.
  const uint8_t *jpeg() const { return jpeg_ && jpegLoaded_ ? jpeg_ : nullptr; }
  size_t jpegBytes() const { return jpegLoaded_ ? timing_.bytes : 0; }
  void close();
  const Timing &timing() const { return timing_; }
  static const char *modeName(Mode mode) {
    return mode == Mode::Reduced ? "reduced" : mode == Mode::Buffered ? "buffered" : "legacy";
  }

private:
  JPEGDEC *decoder_ = nullptr;
  uint8_t *jpeg_ = nullptr;
  uint16_t *pixels_ = nullptr;
  size_t jpegCapacity_ = 0, pixelCapacity_ = 0;
  Timing timing_;
  bool cover_ = false, jpegLoaded_ = false;
  bool reduced(const uint8_t *data, int x, int y, int maxW, int maxH);
};
} // namespace display
} // namespace hal
