#include "apps/agent/agent_motion.h"
#include "config/screen_chrome_layout.h"
#include "apps/agent/agent_screens.h"

#include <Arduino.h>

#include "apps/agent/agent_config.h"
#include "apps/agent/agent_flow.h"
#include "apps/agent/agent_render.h"
#include "apps/agent_app.h"
#include "apps/camera_menu_app.h"
#include "apps/voice_recorder_app.h"
#include "apps/menu_app.h"
#include "services/eyes_asset.h"
#include "hal/hal_display.h"
#include "hal/hal_imu.h"
#include "hal/hal_input.h"
#include "hal/hal_wifi.h"
#include "nav_handler.h"
#include "services.h"
#include "ui/home_eyes.h"
#include "ui/kami_motion.h"
#include "ui/home_motion.h"
#include "core/dizzy_feedback.h"
#include "services/vibration.h"

#include <esp_random.h>

namespace apps {
namespace agent {

namespace {

// Eyes screen: the agent home. Animated Kami eyes.
//
// Input contract:
//   Rotary       → push the app menu
//   Lone A tap   → push Audio Recorder
//   Lone B tap   → push Camera
//   C tap        → no action while awake (C wakes in the sleep service)
//   C hold       → battery sleep via the shared input/power handler
//   A+B chord    → start a PTT recording immediately (release of *both*
//                  buttons ends the take; see screen_recording.cpp)
//
// The chord-hold deadline (chordHoldDeadlineMs) is preserved in onTick for the
// case where screen_reply::onChord returns to Eyes and arms a hold-to-record
// gesture — see screen_reply.cpp.

static ui::home_motion::Model character;
static core::haptic::DizzyFeedback dizzy;
static uint32_t motionAt=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
static uint32_t testShakeUntil=0;
#endif
static bool gravityReady=false;
static float gravity[3]={0,0,0};
static constexpr uint32_t kConnHintWarmupMs=8000;
static uint32_t sConnHintEnterMs=0,sConnReadyAtMs=0;
static bool sConnWasReady=false;

void onTick(AgentState& s, uint32_t nowMs) {
    if (services::eyes_asset::takeInstalledNotification()) {
        Serial.println("[eyes-asset] rebuilding eye sprites from restored bitmap");
        hal::display::releaseSpiForSd();
        ui::home_eyes::endSession();
        render::paintEyesHomeStatic(s);
        hal::display::markAllDirty();
    }

    // A delegation result that arrived while the eyes were showing.
    if (flow::showInboxReply()) return;

    // The chord-hold deadline lives in onTick because it must keep checking
    // each frame whether both buttons are still down.
    if (s.chordHoldDeadlineMs != 0 && (int32_t)(nowMs - s.chordHoldDeadlineMs) >= 0) {
        s.chordHoldDeadlineMs = 0;
        if (hal::input::buttonADown() && hal::input::buttonBDown()) {
            s.recStartedByChord = true;
            flow::startRecording();
            return;
        }
    }

    if ((nowMs - s.lastAnim) >= 16u) {
        s.animFrame++;
        s.lastAnim = nowMs;

        const float dt=(nowMs-motionAt)/1000.f;motionAt=nowMs;
        int32_t accel[3];
        float roll=character.roll,pitch=character.pitch,jolt=0;
        if(ui::kami_motion::acceleration(nowMs,accel)){
            float blend=1-expf(-dt/.08f);
            if(!gravityReady){for(int i=0;i<3;++i)gravity[i]=accel[i];gravityReady=true;}
            for(int i=0;i<3;++i){float d=accel[i]-gravity[i];jolt+=d*d;gravity[i]+=d*blend;}
            jolt=sqrtf(jolt)/1000.f;
            // Existing board mapping: screen-right +Y, screen-down -X, normal Z.
            float planar=hypotf(gravity[1],gravity[0]);
            if(planar>180)roll=atan2f(gravity[1],-gravity[0]);
            pitch=ui::home_motion::limit(atan2f(-gravity[2],fmaxf(1,planar)),-.96f,.96f);

        }
#ifdef ORIGAMI_DEV_SERIAL_INPUT
        if(testShakeUntil && int32_t(testShakeUntil-nowMs)>0)jolt=4;
#endif
        character.step(dt,roll,pitch,jolt);
        s.eyeFloatX=character.x+character.gazeX;s.eyeFloatY=character.y+character.gazeY;
        ui::home_eyes::setHomeMotion(character.squeeze,character.contact);
        const bool settling=character.squeeze<.95f || character.now-character.lastMove>=.18f;
        const bool strong=character.unmoored && character.agitation>=1.6f;
        services::vibration::dizzyFeedback(dizzy.update(nowMs,strong,settling));

        render::serviceEyesShellTick(s);
        // Bottom hint follows the actual agent transport during warmup, but
        // recording is allowed before the socket is ready because audio buffers
        // locally. Do not leave "Connecting..." pinned forever when WiFi is up
        // and the user can start a buffered recording.
        const bool ready = flow::isTransportReady();
        if (ready && !sConnWasReady) sConnReadyAtMs = nowMs;
        sConnWasReady = ready;

        const bool wifiReady = hal::wifi::connected() && !hal::wifi::suspended();
        int hintMode = 0;
        if (ready) {
            hintMode = ((nowMs - sConnReadyAtMs) < 2000u) ? 2 : 0;
        } else if (!wifiReady || (uint32_t)(nowMs - sConnHintEnterMs) < kConnHintWarmupMs) {
            hintMode = 1;
        }
        render::paintHomeConnStatus(hintMode == 0 ? 3 : hintMode);
    }
}

void onInput(AgentState& s, hal::input::InputEvent ev) {
    using ET = hal::input::EventType;
    // C is reserved for wake/hold-to-sleep on Home. In particular, releasing
    // a short or cancelled hold must never enable the old volume shortcut.
    if (ev.type == ET::ButtonC_Tap) {services::vibration::suppressInputFeedback();return;}

    if (ev.type == ET::Scroll && ev.delta != 0) {
        NavHandler::instance().push(&apps::menu::app);
        return;
    }
    if (ev.type == ET::ButtonA_Tap) {
        services::notifyUserActivity();
        NavHandler::instance().push(&apps::voice_recorder::app);
        return;
    }
    if (ev.type == ET::ButtonB_Tap) {
        NavHandler::instance().push(&apps::camera_menu::app);
        return;
    }
}

bool onChord(AgentState& s) {
    // A+B chord on Eyes starts a PTT recording immediately. The take ends only
    // when BOTH buttons are released (screen_recording::onInput enforces this
    // via recStartedByChord).
    s.recStartedByChord = true;
    flow::startRecording();
    return true;
}

void tickScreen(uint32_t nowMs) {
    if (!motion::tick(flow::state(),nowMs)) onTick(flow::state(), nowMs);
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

void stopEyesFeedback(){
    dizzy=core::haptic::DizzyFeedback{};
    services::vibration::dizzyFeedback(false);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    testShakeUntil=0;
#endif
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool testEyesShake(uint32_t duration){
    if(!duration || duration>10000 || NavHandler::instance().top()!=&app || app.currentScreen!=&kScreenEyes)return false;
    testShakeUntil=millis()+duration;return true;
}
#endif

void prepareEyesView(AgentState& s) {
    stopEyesFeedback();
    character=ui::home_motion::Model{};motionAt=millis();gravityReady=false;
    ui::home_eyes::setHomeMotion(0,false);
    // paintEyesHomeStatic already resets physics fields and offset to (0,0).
    render::paintEyesHomeStatic(s);
    s.miniEyesOpen = true;
    sConnHintEnterMs = millis();
    sConnReadyAtMs = 0;
    sConnWasReady = false;
}

const ::scene::Screen kScreenEyes = {
    /*name   */ "Eyes",
    /*onEnter*/ enterEyesScreen,
    /*onExit */ stopEyesFeedback,
    /*onTick */ tickScreen,
    /*onInput*/ inputScreen,
    /*onChord*/ chordScreen,
    /*onDraw */ nullptr,
    /*id     */ "eyes",
    /*builder*/ render::buildEyesScene,
    &screen_chrome_layout::kKamiEyesTopBarColor,
};

}  // namespace agent
}  // namespace apps
