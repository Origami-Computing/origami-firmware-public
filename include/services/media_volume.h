#pragma once
/**
 * @file services/media_volume.h
 * @brief Shared media settings, persistence and route-aware playback gain.
 */

#include <stdint.h>

namespace services {
namespace audio {
namespace media_volume {

// Final digital amplitude ceiling for every route that feeds the speaker.
// Profiles already express final gain: do not multiply their output by 0.20 again.
constexpr uint16_t kSpeakerMaxGainBasisPoints = 2000;

struct VolumeProfile {
    uint16_t minGainBasisPoints;
    uint16_t maxGainBasisPoints;
    uint8_t steps;

    VolumeProfile(uint16_t minBp = 100, uint16_t maxBp = 10000,
                  uint8_t stepCount = 16)
        : minGainBasisPoints(minBp), maxGainBasisPoints(maxBp), steps(stepCount) {}
};

// Task context only. Reads and in-memory setters take coherent snapshots;
// first use may wait for another task's NVS load. Saves belong to the UI task.
// NVS I/O and gain math never run inside the short state critical section.
void load();

int volume();
void setVolume(int vol0to100);
void saveVolume();

VolumeProfile speakerProfile();
VolumeProfile headphoneProfile();
void setSpeakerProfile(VolumeProfile profile);
void setHeadphoneProfile(VolumeProfile profile);
void saveProfiles();

int scaledRouteVolumeBasisPoints(int vol0to100);
int scaledVolumeBasisPoints();

void setGainOverrideBasisPoints(int gainBasisPoints);
int gainOverrideBasisPoints();

}  // namespace media_volume
}  // namespace audio
}  // namespace services
