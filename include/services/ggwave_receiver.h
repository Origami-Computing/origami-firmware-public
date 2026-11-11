#pragma once
#include <stdint.h>

namespace services { namespace ggwave_receiver {
// UI-task API. Owns reservation, temporary radio pause, microphone and decoder
// worker. No persistence; radio restoration follows audio cleanup.
// start/stop are idempotent. stop joins cooperatively before releasing hardware;
// failed start unwinds all resources. Snapshot returns owned data, never pointers.
struct Snapshot {
    bool listening = false;
    bool receiving = false;
    uint32_t messages = 0;
    uint32_t samples = 0;
    uint32_t failures = 0;
    uint16_t peak = 0;
    uint32_t starts = 0;
    uint32_t clipped = 0;
    uint32_t maxDecodeUs = 0;
    int protocol = -1;
    // Relative FFT band powers, not calibrated sound-pressure measurements.
    float lowDb = -120, midDb = -120, highDb = -120;
    char text[141] = {};
};
// Sensitive mode suppresses plaintext snapshots/logging. Consume via takePayload.
bool start(bool sensitive=false);
// UI task only; capacity must be >=141. Takes and clears the private mailbox.
// stop also clears it. Sender packets must be separated to allow consumption.
bool takePayload(char* out, unsigned capacity);
void stop();
Snapshot snapshot();
const char* error(); // UI-task borrowed static string; updated by start().
} }
