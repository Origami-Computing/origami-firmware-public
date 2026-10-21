#include "core/boot_animation_player.h"

#include <string.h>

#include "hal/hal_display.h"
#include "hal/hal_sd.h"

namespace core {
namespace boot_animation {
namespace {

static constexpr uint16_t kVersion = 1;
static constexpr uint16_t kHeaderBytes = 32;
static constexpr uint16_t kFrameHeaderBytes = 20;
static constexpr uint8_t kPixelFormatRgb565 = 1;
static constexpr uint8_t kEncodingRaw = 0;
static constexpr uint8_t kEncodingRle16 = 1;
static constexpr uint32_t kMaxFrames = 1800;
static constexpr uint32_t kMaxDurationMs = 60000;

struct FrameHeader {
  uint32_t timestampMs = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t encoding = 0;
  uint32_t payloadBytes = 0;
};

static hal::sd::StreamReader s_reader;
static uint8_t s_buffer[1024];
static size_t s_bufferAt = 0;
static size_t s_bufferSize = 0;
static FrameHeader s_next;
static bool s_open = false;
static bool s_hasNext = false;
static bool s_failed = false;
static const char* s_error = "not opened";
static uint32_t s_durationMs = 0;
static uint32_t s_holdMs = 0;
static uint32_t s_frameCount = 0;
static uint32_t s_framesRead = 0;
static uint32_t s_framesRendered = 0;
static uint32_t s_lastTimestamp = 0;

static uint16_t le16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t framebufferPixel(uint16_t color) {
  return (uint16_t)((color >> 8) | (color << 8));
}

static bool fail(const char* message) {
  s_failed = true;
  s_error = message;
  s_hasNext = false;
  if (s_reader.handle) hal::sd::closeStream(&s_reader);
  s_open = false;
  Serial.printf("[boot-oani] error: %s\n", message);
  return false;
}

static bool readBytes(uint8_t* destination, size_t len) {
  while (len > 0) {
    if (s_bufferAt == s_bufferSize) {
      s_bufferSize = hal::sd::readStream(&s_reader, s_buffer, sizeof(s_buffer));
      s_bufferAt = 0;
      if (s_bufferSize == 0) return false;
    }
    const size_t available = s_bufferSize - s_bufferAt;
    const size_t take = min(available, len);
    memcpy(destination, s_buffer + s_bufferAt, take);
    destination += take;
    s_bufferAt += take;
    len -= take;
  }
  return true;
}

static bool loadNextHeader() {
  if (s_framesRead >= s_frameCount) {
    s_hasNext = false;
    return true;
  }
  uint8_t raw[kFrameHeaderBytes];
  if (!readBytes(raw, sizeof(raw))) return fail("truncated frame header");
  FrameHeader frame;
  frame.timestampMs = le32(raw + 0);
  frame.x = le16(raw + 4);
  frame.y = le16(raw + 6);
  frame.width = le16(raw + 8);
  frame.height = le16(raw + 10);
  frame.encoding = raw[12];
  frame.payloadBytes = le32(raw + 16);
  if (frame.timestampMs > s_durationMs ||
      (s_framesRead > 0 && frame.timestampMs < s_lastTimestamp)) {
    return fail("invalid frame timestamp");
  }
  if (frame.width == 0 || frame.height == 0 ||
      frame.x + frame.width > hal::display::kCanvasW ||
      frame.y + frame.height > hal::display::kCanvasH) {
    return fail("invalid frame bounds");
  }
  const uint32_t pixels = (uint32_t)frame.width * frame.height;
  if (frame.encoding == kEncodingRaw) {
    if (frame.payloadBytes != pixels * 2u) return fail("invalid raw frame size");
  } else if (frame.encoding == kEncodingRle16) {
    if (frame.payloadBytes < 4u || (frame.payloadBytes & 3u) != 0u ||
        frame.payloadBytes > pixels * 4u) {
      return fail("invalid RLE frame size");
    }
  } else {
    return fail("unsupported frame encoding");
  }
  if (s_framesRead == 0 &&
      (frame.timestampMs != 0 || frame.x != 0 || frame.y != 0 ||
       frame.width != hal::display::kCanvasW || frame.height != hal::display::kCanvasH)) {
    return fail("first frame is not a full keyframe");
  }
  s_next = frame;
  s_hasNext = true;
  return true;
}

static bool applyRaw(TFT_eSprite& canvas, const FrameHeader& frame) {
  uint16_t* framebuffer = static_cast<uint16_t*>(canvas.getPointer());
  if (!framebuffer) return fail("framebuffer unavailable");
  uint8_t pair[2];
  for (uint16_t row = 0; row < frame.height; ++row) {
    uint16_t* out = framebuffer + (size_t)(frame.y + row) * hal::display::kCanvasW + frame.x;
    for (uint16_t column = 0; column < frame.width; ++column) {
      if (!readBytes(pair, sizeof(pair))) return fail("truncated raw payload");
      out[column] = framebufferPixel(le16(pair));
    }
  }
  return true;
}

static bool applyRle(TFT_eSprite& canvas, const FrameHeader& frame) {
  uint16_t* framebuffer = static_cast<uint16_t*>(canvas.getPointer());
  if (!framebuffer) return fail("framebuffer unavailable");
  const uint32_t expectedPixels = (uint32_t)frame.width * frame.height;
  const uint32_t runs = frame.payloadBytes / 4u;
  uint32_t pixelIndex = 0;
  uint8_t runData[4];
  for (uint32_t run = 0; run < runs; ++run) {
    if (!readBytes(runData, sizeof(runData))) return fail("truncated RLE payload");
    const uint16_t count = le16(runData);
    const uint16_t color = framebufferPixel(le16(runData + 2));
    if (count == 0 || pixelIndex + count > expectedPixels) return fail("invalid RLE run");
    for (uint16_t n = 0; n < count; ++n, ++pixelIndex) {
      const uint32_t row = pixelIndex / frame.width;
      const uint32_t column = pixelIndex - row * frame.width;
      framebuffer[(size_t)(frame.y + row) * hal::display::kCanvasW + frame.x + column] = color;
    }
  }
  return pixelIndex == expectedPixels ? true : fail("incomplete RLE frame");
}

static bool applyFrame(TFT_eSprite& canvas) {
  const FrameHeader frame = s_next;
  const bool ok = frame.encoding == kEncodingRaw
      ? applyRaw(canvas, frame) : applyRle(canvas, frame);
  if (!ok) return false;
  ++s_framesRead;
  ++s_framesRendered;
  s_lastTimestamp = frame.timestampMs;
  if (frame.y == 0 && frame.height == hal::display::kCanvasH) hal::display::markAllDirty();
  else hal::display::markDirty(frame.y, frame.height);
  return loadNextHeader();
}

}  // namespace

bool begin(const char* path) {
  end();
  s_failed = false;
  s_error = "missing";
  if (!hal::sd::mounted() || !hal::sd::openStream(path, &s_reader)) return false;

  s_bufferAt = 0;
  s_bufferSize = 0;
  uint8_t header[kHeaderBytes];
  if (!readBytes(header, sizeof(header))) return fail("truncated header");
  if (memcmp(header, "OANI", 4) != 0) return fail("bad magic");
  if (le16(header + 4) != kVersion || le16(header + 6) != kHeaderBytes) {
    return fail("unsupported version");
  }
  if (le16(header + 8) != hal::display::kCanvasW ||
      le16(header + 10) != hal::display::kCanvasH ||
      header[24] != kPixelFormatRgb565) {
    return fail("unsupported canvas or pixel format");
  }
  s_frameCount = le32(header + 12);
  s_durationMs = le32(header + 16);
  s_holdMs = le32(header + 20);
  if (s_frameCount == 0 || s_frameCount > kMaxFrames ||
      s_durationMs == 0 || s_durationMs > kMaxDurationMs || s_holdMs > 10000u) {
    return fail("unsafe animation limits");
  }
  s_framesRead = 0;
  s_framesRendered = 0;
  s_lastTimestamp = 0;
  s_open = true;
  if (!loadNextHeader()) return false;
  s_error = "ok";
  Serial.printf("[boot-oani] opened %s frames=%lu duration=%lu hold=%lu bytes=%lu\n",
                path, (unsigned long)s_frameCount, (unsigned long)s_durationMs,
                (unsigned long)s_holdMs, (unsigned long)s_reader.size);
  return true;
}

void end() {
  if (s_reader.handle) hal::sd::closeStream(&s_reader);
  s_open = false;
  s_hasNext = false;
}

bool active() { return s_open; }
bool failed() { return s_failed; }
const char* error() { return s_error; }
uint32_t durationMs() { return s_durationMs; }
uint32_t holdMs() { return s_holdMs; }
uint32_t frameCount() { return s_frameCount; }
uint32_t renderedFrameCount() { return s_framesRendered; }

bool renderDue(TFT_eSprite& canvas, uint32_t elapsedMs) {
  if (!s_open) return !s_failed;
  while (s_hasNext && s_next.timestampMs <= elapsedMs) {
    if (!applyFrame(canvas)) return false;
  }
  return true;
}

}  // namespace boot_animation
}  // namespace core
