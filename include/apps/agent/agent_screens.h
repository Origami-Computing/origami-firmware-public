#pragma once

#include <stdint.h>

#include "apps/agent/agent_state.h"
#include "hal/hal_input.h"
#include "scene/screen.h"

namespace apps {
namespace agent {

// ── Concrete screen tables (defined in src/apps/agent/screen_*.cpp) ──────────

extern const ::scene::Screen kScreenEyes;
extern const ::scene::Screen kScreenRecording;
extern const ::scene::Screen kScreenThinking;
extern const ::scene::Screen kScreenReply;
extern const ::scene::Screen kScreenError;

// Eyes view initialization; the app entry callback follows it with service work.
void prepareEyesView(AgentState& state);
void enterEyesScreen();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool testEyesShake(uint32_t duration);
#endif
void stopEyesFeedback(); // UI task: release home animation motor on cover/exit/sleep

/** Map a stage enum to its native scene screen. Never null. */
const ::scene::Screen& appScreenFor(Stage stage);

/** Shared per-frame service run after the active Kami screen tick. */
void serviceScreenTick(uint32_t nowMs);

/** Shared input pre-processing run before the active Kami screen handler. */
void prepareScreenInput(hal::input::InputEvent ev);

}  // namespace agent
}  // namespace apps
