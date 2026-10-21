#pragma once
namespace services { namespace shipping {
enum class State { Unsupported, Normal, Locked, Fault };
const char* name(State state);
// Dedicated NVS partition; never erases on corruption or initialization failure.
State state();
// Setup task before board bringup. Only returns for normal or deliberate unlock.
// True means the shipping gate authorized this boot; skip ordinary C-only gate.
bool bootGate();
// UI task AFTER terminal app teardown. False means no lock committed and caller
// can restore navigation. Success does not return; no nav/UI imports here.
bool enter();
} }
