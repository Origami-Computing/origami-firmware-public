#pragma once

namespace services { namespace diagnostics {
// UI-task 440 Hz / 44.1 kHz diagnostic tone, using the existing codec profile.
// The app owns the hold/release workflow; this operation owns codec/TX setup,
// oscillator state and one blocking 256-sample stereo write per requested tick.
// Setup remains best-effort: call writeBlock only while the workflow requests
// tone output, then stop (or restore UI playback) when leaving that operation.
class Tone {
public:
    Tone() = default;
    Tone(const Tone&) = delete;
    Tone& operator=(const Tone&) = delete;
    void start();
    void writeBlock();
    // Preserves legacy unconditional TX teardown, including before mic setup.
    void stop();
private:
    float phase_ = 0.0f;
};
} }
