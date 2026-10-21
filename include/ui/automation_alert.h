#pragma once

#include "scene/screen.h"

namespace ui {

/** System-owned full-screen automation alert scene. */
extern const ::scene::Screen kScreenAutomationAlert;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void printAlertAudioStatus();
#endif

}  // namespace ui
