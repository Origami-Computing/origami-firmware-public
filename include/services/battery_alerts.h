#pragma once
#include <stdint.h>
namespace services { namespace battery_alerts {
// UI-task battery cue coordinator: debouncing is pure core policy; SFX owns
// codec/TX admission and idle teardown. Preserves synchronous one-shot playback.
// Call once per UI loop, independently of status-bar visibility or painting.
void service(uint32_t now);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Explicit audio fixture only; expires after two minutes. Never writes telemetry.
void setPreview(int percent, int power, bool audio);
struct Diagnostics {
    uint32_t low, plug, skipped, frames;
    bool fixtureAudio;
    int volume;
};
Diagnostics diagnostics();
#endif
} }
