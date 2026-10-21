#pragma once
#include "app.h"

namespace apps { namespace safe_area {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
extern App app;
bool isActive();
// Consumes only the safearea command family. Called on ui_task.
bool handleCommand(const char* line);
#else
inline bool isActive() { return false; }
#endif
}}
