#include "services/camera_capture_policy.h"

using namespace services::camera::capture_policy;

constexpr uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
constexpr uint8_t incomplete[] = {0xff, 0xd8, 0x00, 0x00};
static_assert(validVgaJpeg(jpeg, sizeof(jpeg), 640, 480), "VGA envelope");
static_assert(!validVgaJpeg(nullptr, 4, 640, 480), "null buffer");
static_assert(!validVgaJpeg(jpeg, 0, 640, 480), "empty buffer");
static_assert(!validVgaJpeg(jpeg, 3, 640, 480), "short buffer");
static_assert(!validVgaJpeg(jpeg, kMaxJpegBytes + 1, 640, 480), "oversize before dereference");
static_assert(!validVgaJpeg(incomplete, sizeof(incomplete), 640, 480), "truncated JPEG");
static_assert(!validVgaJpeg(jpeg, sizeof(jpeg), 320, 240), "reject preview resolution");
static_assert(!validVgaJpeg(jpeg, sizeof(jpeg), 1280, 720), "reject HD on VGA path");
static_assert(!freshFrame(0, 0), "unknown timestamp");
static_assert(!freshFrame(99, 100), "stale frame");
static_assert(freshFrame(100, 100), "frame boundary");
static_assert(freshFrame(101, 100), "fresh frame");
static_assert(freshFrame(0x100000001ULL, 0x100000000ULL), "64-bit timestamps");
static_assert(!freshFrame(0xffffffffULL, 0x100000000ULL), "no 32-bit wrap");
