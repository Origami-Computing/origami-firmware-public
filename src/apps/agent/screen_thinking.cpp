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
// A/B still down from the turn that led here (the server's endpoint can end a
// push-to-talk turn while A+B are held). Letting go of them is not a cancel;
// only a button pressed after Thinking began can cancel.
static bool sHeldFromTurn = false;

void onEnter(AgentState& s) {
    sThinkingEnteredMs = millis();
    sHeldFromTurn = hal::input::buttonADown() || hal::input::buttonBDown();
    s.thinkDotPhase = 255;  // force first repaint
    snprintf(s.thinkStatus, sizeof(s.thinkStatus), "%s", "Connecting agent");
    motion::cancel();
    render::paintThinkingShell(s,0);
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
    // The painter skips unchanged frames; the eyes glance and then look around.
    render::paintThinkingShell(s, phase);
    flow::pollThinking(sThinkingEnteredMs);
}

void onInput(AgentState& s, hal::input::InputEvent ev) {
    // Lone A press: abort to eyes home. B press: abort to menu.
    using ET = hal::input::EventType;
    if (ev.type == ET::ButtonC_Tap) return;   // C keeps its OS meaning only
    if (sHeldFromTurn) {
        if (ev.type != ET::ButtonA_Press && ev.type != ET::ButtonB_Press) return;
        sHeldFromTurn = false;   // a fresh press: normal cancel handling resumes
    }
    if (s.handsFree) s.handsFreeReady = false;
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
