#include "config/screen_chrome_layout.h"
#include "apps/agent/agent_screens.h"

#include <Arduino.h>

#include "apps/agent/agent_flow.h"
#include "apps/agent/agent_render.h"
#include "hal/hal_input.h"

namespace apps {
namespace agent {

namespace {

// Full-size Kami and amplitude border; the voice service drains capture before rendering.
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
    // The border also crosses the status strip, which content clears preserve.
    render::paintRecordingBorder(s, millis(), true);
}

void onInput(AgentState& s, hal::input::InputEvent ev) {
    using ET = hal::input::EventType;
    if (ev.type != ET::ButtonA_Release && ev.type != ET::ButtonB_Release) return;

    // Chord-initiated recording: wait until BOTH buttons are released, otherwise
    // the first chord-release would end the recording immediately (the user
    // didn't even speak yet).
    if (s.recStartedByChord) {
        if (hal::input::buttonADown() || hal::input::buttonBDown()) return;
    }
    flow::finishRecording();
}

bool onChord(AgentState& /*s*/) {
    return true;  // chord while recording is a no-op
}

void enterScreen() {
    onEnter(flow::state());
}

void exitScreen() {
    onExit(flow::state());
}

void tickScreen(uint32_t nowMs) {
    if (flow::pollCompletion()) return;
    if(!flow::wifiReady()) { flow::finishRecording(); return; }
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
