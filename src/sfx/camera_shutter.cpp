#include "sfx/camera_shutter.h"

namespace sfx {
namespace {
const int16_t kSamples[] = {
#include "camera_shutter_pcm.inc"
};
}
const PcmClip kCameraShutter = {kSamples, sizeof(kSamples) / sizeof(kSamples[0])};
}
