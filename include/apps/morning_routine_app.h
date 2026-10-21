#pragma once

#include "app.h"

namespace apps {
namespace morning_routine {

extern App app;

#ifdef ORIGAMI_DEV_SERIAL_INPUT
/** Developer-only launcher that starts the complete alarm-to-reflection flow. */
extern App testApp;
/** Bench-only RAM quote previews and one-shot queue-failure injection. */
bool developmentUi(const char* command);
#endif

bool isActive();
bool isRecording();

}  // namespace morning_routine
}  // namespace apps
