#pragma once

#include "app.h"

namespace scene {
struct Screen;
}

namespace apps {
namespace boot {

// Called once when the Boot view finishes. Replace the startup root with Agent;
// when opened above another app, return to that app. Owns handoff timing logs.
void completeAnimation(uint32_t now, uint32_t startedMs, uint32_t scheduledMs);

extern App app;
extern const ::scene::Screen kScreenBoot;

}  // namespace boot
}  // namespace apps
