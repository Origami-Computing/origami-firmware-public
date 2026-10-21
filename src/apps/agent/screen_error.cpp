#include "config/screen_chrome_layout.h"
#include "apps/agent/agent_screens.h"

#include <Arduino.h>

#include "apps/agent_app.h"
#include "services/vibration.h"
#include "apps/agent/agent_config.h"
#include "apps/agent/agent_flow.h"
#include "apps/agent/agent_render.h"
#include "hal/hal_input.h"

namespace apps {
namespace agent {

namespace {

// Local, persistent notices. No network request or typewriter is needed.
void onEnter(AgentState& s) {
    s.noticeSince=millis();s.noticeInput.reset();s.recStartedByChord=false;
    s.noticeKind=notice::classify(s.errBuf,flow::wifiReady());
    render::paintError(s);
    // One short pulse; no repeating error sound or animation.
    const core::alert_vibration::Pattern tap={1,1,"Agent notice",{{35,165}},0};
    if(!services::vibration::previewing())services::vibration::previewCustom(tap,100);
}
void onExit(AgentState& s) { s.errBuf[0]='\0'; }
void onTick(AgentState&,uint32_t nowMs) { flow::serviceNotice(nowMs); }
void onInput(AgentState& s,hal::input::InputEvent ev) {
    using ET=hal::input::EventType;
    if(!s.noticeInput.armed)return;
    if(ev.type==ET::ButtonA_Tap){flow::dismissNotice();return;}
    if(ev.type==ET::ButtonB_Tap && (notice::needsWifi(s.noticeKind)||s.noticeKind==notice::Kind::Auth)){
        flow::openNoticeSettings();return;
    }
}
bool onChord(AgentState& s) {
    if(!s.noticeInput.armed)return true;
    s.recStartedByChord=true;flow::startRecording();return true;
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

const ::scene::Screen kScreenError = {
    /*name   */ "Error",
    /*onEnter*/ enterScreen,
    /*onExit */ exitScreen,
    /*onTick */ tickScreen,
    /*onInput*/ inputScreen,
    /*onChord*/ chordScreen,
    /*onDraw */ nullptr,
    /*id     */ "error",
    /*builder*/ render::buildErrorScene,
    &screen_chrome_layout::kKamiErrorTopBarColor,
};

}  // namespace agent
}  // namespace apps
