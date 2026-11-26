#include "services/agent_transport.h"
#include "config/screen_chrome_layout.h"
#include "apps/agent/agent_screens.h"

#include <Arduino.h>

#include "apps/agent/agent_flow.h"
#include "apps/agent/agent_render.h"
#include "hal/hal_input.h"

namespace apps {
namespace agent {

namespace {

// Eyes and your one-line transcript; the voice service drains capture before rendering.
// The screen handles the release-to-send transition and
// honors chord vs single-button release semantics so a chord-initiated recording
// only ends when BOTH buttons are up.

void onEnter(AgentState& s) {
    // Acknowledge the gesture before mic startup; capture callbacks remain idle
    // until startRecording() starts the new stream.
    if (s.conv.held() || s.spritesReady) {
        render::releaseChatSprites(s);
    }
    render::paintRecDot(s, AgentState::RecDot::Off, millis());
    render::beginRecordingView(s, millis());
}

void onExit(AgentState& s) {
    // Release the flush-time microphone servicing hook on view exit.
    render::paintRecordingBorder(s, millis(), true);
}

void onInput(AgentState& s, hal::input::InputEvent ev) {
    using ET = hal::input::EventType;
    // C keeps its OS meaning (hold to sleep); a short tap does nothing here.
    if (ev.type == ET::ButtonC_Tap) return;
    if (ev.type == ET::Scroll && render::scrollListeningReply(s, ev.delta)) return;
    if (s.continuous) {
        // Agent mode: A closes an open document first, otherwise leaves.
        if (s.documentVisible && (ev.type == ET::ButtonA_Tap || ev.type == ET::ButtonB_Tap)) {
            s.documentVisible = false; s.documentId[0] = 0; s.transcriptPainted = UINT32_MAX;
            services::agent_transport::closeDocument(); return;
        }
        if (ev.type == ET::ButtonA_Tap) flow::leaveAgentMode();
        return;
    }
    if (ev.type != ET::ButtonA_Release && ev.type != ET::ButtonB_Release) return;

    // Push-to-talk started by A+B ends only when BOTH buttons are up; the
    // first release would otherwise end the take before you spoke.
    if (s.recStartedByChord) {
        if (hal::input::buttonADown() || hal::input::buttonBDown()) return;
        if (flow::takeTapAsAgentMode(millis())) return;   // quick release: agent mode
    }
    flow::finishRecording();
}

bool onChord(AgentState& s) {
    // A+B tapped again in agent mode leaves it; during push-to-talk it is a no-op.
    if (s.continuous) flow::leaveAgentMode();
    return true;
}

void enterScreen() {
    onEnter(flow::state());
}

void exitScreen() {
    onExit(flow::state());
}

void tickScreen(uint32_t nowMs) {
    if (flow::pollCompletion()) return;
    if(flow::state().endpointRequested) {
        flow::state().endpointRequested=false;
        flow::state().suppressHeldChord=true;
        flow::finishRecording();return;
    }
    if(!flow::wifiReady()) { flow::finishRecording(); return; }
    flow::serviceTurnGesture(nowMs);
    flow::serviceAgentSilence(nowMs);
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

const ::scene::Screen kScreenRecording = {
    /*name   */ "Recording",
    /*onEnter*/ enterScreen,
    /*onExit */ exitScreen,
    /*onTick */ tickScreen,
    /*onInput*/ inputScreen,
    /*onChord*/ chordScreen,
    /*onDraw */ nullptr,
    /*id     */ "recording",
    /*builder*/ render::buildRecordingScene,
    &screen_chrome_layout::kKamiRecordingTopBarColor,
};

}  // namespace agent
}  // namespace apps
