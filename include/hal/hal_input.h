#pragma once

#include <Arduino.h>

namespace hal {
namespace input {

// ---------------------------------------------------------------------------
// InputEvent — unified input abstraction
// ---------------------------------------------------------------------------

enum class EventType : uint8_t {
    None,
    ButtonA_Press,    // physical LB / left button
    ButtonA_Release,
    ButtonA_Tap,      // pressed and released with no chord during that down window
    ButtonB_Press,    // physical RB / right button
    ButtonB_Release,
    ButtonB_Tap,      // pressed and released with no chord during that down window
    ButtonC_Press,    // encoder press
    ButtonC_Release,
    ButtonC_Tap,
    Chord_AB,
    Scroll,           // encoder clockwise / counterclockwise
    TextInput,        // ASCII character from the USB serial keyboard bridge
    KeyEnter,
    KeyBackspace,
    KeyDelete,
    KeyLeft,
    KeyRight,
    KeyHome,
    KeyEnd,
    PowerSleep       // C reaches power_button::kHoldMs; never forwarded as a click
};

struct InputEvent {
    EventType type  = EventType::None;
    int16_t   delta = 0;
    uint8_t   key   = 0;
    uint32_t rotaryGesture = 0; // Identifies an early scroll and its possible rollback.
    bool rotaryUndo = false;
    uint32_t traceId = 0; // Diagnostic correlation only; zero for untagged inputs.
};

/**
 * Sample all physical inputs (MCP23017 buttons via hal::io, rotary encoder),
 * run edge detection / chord detection / hold-timer, and enqueue InputEvents.
 * Call once per main-loop iteration, before poll().
 */
void service(uint32_t nowMs);

/** Dequeue the next InputEvent. Returns true if an event was available. */
bool poll(InputEvent& out);

#ifdef ORIGAMI_DEV_SERIAL_INPUT
/** OR virtual A/B levels into the sampled pins: bit 0=A, bit 1=B; zero releases. */
void setTestButtons(uint8_t mask);
#endif

/** Developer/test hook: enqueue a synthetic input event through the normal input path. */
void injectEvent(EventType type, int16_t delta = 0, uint8_t key = 0);

/** True while button A is physically held (regardless of chord state). */
bool buttonADown();
/** True while button B is physically held (regardless of chord state). */
bool buttonBDown();
/** True while encoder button C is physically held. */
bool buttonCDown();
/** Discard the wake gesture and any held A/B until all buttons are released. */
void suppressUntilButtonsReleased();

// ---------------------------------------------------------------------------
// Rotary encoder (existing API, kept for direct access)
// ---------------------------------------------------------------------------

void configureRotaryEncoderPins();
void resetRotaryEncoderPins();
int readRotaryA();
int readRotaryB();
void beginRotaryInterrupts();
int takeRotaryDetents();
void resyncRotaryQuadrature();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Atomic, non-destructive ISR counters. Synthetic input events do not affect them.
struct RotarySnapshot {
  uint32_t forward,reverse,invalid,resyncs,scrollForward,scrollReverse;
  uint8_t state;int8_t partial;bool ready,anchored;
};
RotarySnapshot rotarySnapshot();
#endif

}  // namespace input
}  // namespace hal
