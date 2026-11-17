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
  bool reduced(int x, int y, int maxW, int maxH);
};
} // namespace display
} // namespace hal
