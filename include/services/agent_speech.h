#pragma once
#include <stdint.h>
namespace services { namespace agent_speech {
// All calls on UI task. Owns bounded network-to-playback staging and the existing
// WavPlayer session. Never starts RX. stop retains player retirement ownership.
void setEnabled(bool enabled);
bool enabled();
bool busy();
void receive(const char* json);
void tick(uint32_t now);
void stop();
} }
