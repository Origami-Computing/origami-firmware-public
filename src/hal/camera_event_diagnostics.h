#pragma once
#include <stddef.h>
#include <stdint.h>

// Development probe only. Called by the exclusive HAL frame owner, and after
// driver deinit (no remaining interrupts), respectively. No retained frame loan.
#if defined(ORIGAMI_CAMERA_EVENT_DIAGNOSTICS)
// After camera driver initialization; optional read-only GPIO edge observer.
// Failure leaves capture operational and reports the observer unavailable.
void cameraEventBegin();
void cameraEventObserve(const uint8_t* bytes, size_t size);
void cameraEventFinish();
#endif
