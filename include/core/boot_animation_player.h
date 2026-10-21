#pragma once

#include <Arduino.h>
#include <TB_TFT_eSPI.h>

namespace core {
namespace boot_animation {

static constexpr const char kDefaultPath[] = "/boot/active.oani";

// Opens and validates an Origami Animation v1 stream. The first frame must be a
// full 280x240 keyframe. Invalid or missing files return false so boot can use
// its compiled-in fallback.
bool begin(const char* path = kDefaultPath);
void end();
bool active();
bool failed();
const char* error();
uint32_t durationMs();
uint32_t holdMs();
uint32_t frameCount();
uint32_t renderedFrameCount();

// Applies every frame whose timestamp is <= elapsedMs. Pixels are streamed from
// SD into the existing RGB565 framebuffer and dirty bands are marked only when
// a frame changes the display. False means decoding failed and fallback should
// take over.
bool renderDue(TFT_eSprite& canvas, uint32_t elapsedMs);

}  // namespace boot_animation
}  // namespace core
