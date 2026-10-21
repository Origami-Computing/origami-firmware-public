#include "apps/agent/agent_motion.h"
#include "config/screen_chrome_layout.h"
#include "apps/agent/agent_screens.h"

#include <Arduino.h>

#include "apps/agent/agent_config.h"
#include "apps/agent/agent_flow.h"
#include "apps/agent/agent_render.h"
#include "hal/hal_input.h"

namespace apps {
namespace agent {

namespace {

// Thinking screen: dialogue box with animated "." / ".." / "...".
//
// WebSocket response events run on kami_ws (Core 0), so ui_task is never blocked
// here. Dots animate through normal onTick and A/B cancel uses normal input.

static uint32_t sThinkingEnteredMs = 0;

void onEnter(AgentState& s) {
    sThinkingEnteredMs = millis();
    s.thinkDotPhase = 255;  // force first repaint
    snprintf(s.thinkStatus, sizeof(s.thinkStatus), "%s", "Connecting agent");
    motion::startThinking();
}

void onExit(AgentState& /*s*/) {
}

void onTick(AgentState& s, uint32_t nowMs) {
    const uint8_t phase = (uint8_t)((nowMs / cfg::agent::kDotStepMs) % 3u);
    const char* status = flow::isCommitSent() ? "Thinking" : "Connecting agent";
    if (strncmp(s.thinkStatus, status, sizeof(s.thinkStatus)) != 0) {
        snprintf(s.thinkStatus, sizeof(s.thinkStatus), "%s", status);
        s.thinkDotPhase = 255;
    }
    if (!motion::tick(s,nowMs) && phase != s.thinkDotPhase) {
        render::paintThinkingShell(s, phase);
    }
    flow::pollThinking(sThinkingEnteredMs);
}

void onInput(AgentState& /*s*/, hal::input::InputEvent ev) {
    // Lone A press: abort to eyes home. B press: abort to menu.
    using ET = hal::input::EventType;
    if (ev.type == ET::ButtonA_Tap || ev.type == ET::ButtonA_Release) {
        flow::cancelSend(CancelTarget::Eyes);
    } else if (ev.type == ET::ButtonB_Press) {
        flow::cancelSend(CancelTarget::Menu);
    }
}

bool onChord(AgentState& /*s*/) {
    flow::cancelSend(CancelTarget::Menu);
    return true;
}

void enterScreen() {
    onEnter(flow::state());
}

void exitScreen() {
    onExit(flow::state());
}

void tickScreen(uint32_t nowMs) {
    onTick(flow::state(), nowMs);
    serviceScreenTick(nowMs);
}

void inputScreen(hal::input::InputEvent ev) {
    prepareScreenInput(ev);
    onInput(flow::state(), ev);
}

bool chordScreen() {
    return onChord(flow::state());
}

}  // namespace

const ::scene::Screen kScreenThinking = {
    /*name   */ "Thinking",
    /*onEnter*/ enterScreen,
    /*onExit */ exitScreen,
    /*onTick */ tickScreen,
    /*onInput*/ inputScreen,
    /*onChord*/ chordScreen,
    /*onDraw */ nullptr,
    /*id     */ "thinking",
    /*builder*/ render::buildThinkingScene,
    &screen_chrome_layout::kKamiThinkingTopBarColor,
};

}  // namespace agent
}  // namespace apps
