#pragma once
#include <stdint.h>
namespace hal { namespace shipping {
struct Inputs { uint8_t buttons; bool usb; };
// Setup/boot task only before normal bringup, or UI task after all owners stop.
// Latches are written before directions; never starts normal TCA defaults.
bool prepare();
bool read(Inputs& out);
bool outputsOff();
// Clears shared interrupts and holds direct pins. A stuck INT uses a bounded
// retry timer instead of immediately waking forever. Never enables product IO.
void sleep(uint32_t retrySeconds = 0);
} }
