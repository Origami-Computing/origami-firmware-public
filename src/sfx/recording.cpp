#include "sfx/recording.h"

namespace sfx {
namespace {
const int16_t kStart[] = {
#include "recording_start_pcm.inc"
};
const int16_t kStop[] = {
#include "recording_stop_pcm.inc"
};
}
const PcmClip kRecordingStart = {kStart, sizeof(kStart) / sizeof(kStart[0])};
const PcmClip kRecordingStop = {kStop, sizeof(kStop) / sizeof(kStop[0])};
}
