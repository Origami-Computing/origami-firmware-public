#include "apps/agent/agent_motion.h"
/**
 * @file nav_handler.cpp
 * @brief NavHandler implementation — app-stack push/pop, chord routing, draw.
 */

#include "nav_handler.h"
#include "core/usage_events.h"
#include <esp_timer.h>
#include "ui/list_view.h"
#include "hal/hal_encoder_trace.h"
#include "services/vibration.h"
#include "core/power_button.h"

#include <Arduino.h>
#include "app.h"
#include "scene/screen.h"
#include "sfx/audio_group.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "sfx/sfx_lib.h"
#include "ui/screen_chrome.h"
#include "ui/focused_label.h"
#include "services/automation.h"
#include "services.h"
#include "config/ui_layout.h"

NavHandler& NavHandler::instance() {
    static NavHandler s;
    return s;
}

namespace {
App* usageForeground = nullptr;
int64_t usageSince = 0;
void usageEnd(App* app, const char* event);
void usageStart(App* app, const char* event) {
 if (!app) return;
 if (usageForeground) usageEnd(usageForeground,"app.cover");
 usageForeground=app; usageSince=esp_timer_get_time()/1000;
 core::usage::emit(app->id ? app->id : "unknown",event);
}
void usageEnd(App* app, const char* event) {
 if (!app) return;
 const int64_t elapsed=usageForeground==app ? esp_timer_get_time()/1000-usageSince : -1;
 core::usage::emit(app->id ? app->id : "unknown",event,elapsed);
 if (usageForeground==app) usageForeground=nullptr;
}
}
static void enterApp(App* app, bool activating = false) {
    ui::focused_label::reset();
    if (!app || !app->initialScreen) return;
    app->currentScreen = app->initialScreen;
    usageStart(app, activating ? "app.enter" : "app.resume");
    if (activating && app->onEnter) app->onEnter();
    if (!activating && app->onResume) app->onResume();
    if (app->currentScreen->onEnter) app->currentScreen->onEnter();
    NavHandler::instance().runtimePolicyChanged();
    // Repaint the status strip once on entry. Previously NavHandler::draw forced
    // a full chrome repaint EVERY frame, which kept the display dirty bits set so
    // flush() pushed the whole framebuffer (VSync wait + 134 KB SPI) every frame
    // — visible as clock flicker, and severe enough that the extra radio/SPI
    // contention during a WiFi (re)connect made the whole UI appear to freeze
    // until the join finished. Invalidating once per transition lets the status
    // bar's own change detection drive repaints; idle frames now no-op flush().
    NavHandler::instance().invalidateChrome();
}

static void exitApp(App* app) {
    if (!app) return;
    if (app->currentScreen && app->currentScreen->onExit) app->currentScreen->onExit();
    usageEnd(app,"app.exit");
    if (app->onExit) app->onExit();
    app->currentScreen = nullptr;
}

void NavHandler::init(App* root) {
    if (shuttingDown_ && depth_ > 0) return;
    shuttingDown_ = false;
    stack_[0] = root;
    depth_ = 1;
    runtimePolicyChanged();
    enterApp(root, true);
    armInputBlock(400);
}

void NavHandler::push(App* app) {
    if (shuttingDown_) return;
    if (!app || depth_ >= kMaxDepth) return;
    services::vibration::navigationFeedback(services::vibration::Cue::Select);
    usageEnd(top(),"app.cover");
    if (top() && top()->onCover) top()->onCover();
    sfx::play(sfx::kSfxOpen);
    stack_[depth_++] = app;
    runtimePolicyChanged();
    enterApp(app, true);
    armInputBlock(120);
}

bool NavHandler::open(App* app) {
    if(shuttingDown_ || !app || !app->initialScreen || !depth_) return false;
    for(int i=0;i<depth_;++i) if(stack_[i]==app) {
        while(depth_>i+1) pop();
        return true;
    }
    if(depth_>=kMaxDepth) return false;
    push(app);return top()==app;
}

void NavHandler::pop() {
    if (shuttingDown_) return;
    if (depth_ <= 1) return;

    App* exiting = stack_[depth_ - 1];
    services::vibration::navigationFeedback(services::vibration::Cue::Back);
    sfx::play(sfx::kSfxBack);
    exitApp(exiting);
    stack_[--depth_] = nullptr;
    runtimePolicyChanged();

    hal::input::resetRotaryEncoderPins();
    hal::input::beginRotaryInterrupts();
    hal::input::resyncRotaryQuadrature();

    enterApp(top());
    armInputBlock(120);
}

void NavHandler::popToRoot() {
    if (shuttingDown_) return;
    if (depth_ <= 1) return;

    services::vibration::navigationFeedback(services::vibration::Cue::Back);
    sfx::play(sfx::kSfxBack);
    while (depth_ > 1) {
        App* exiting = stack_[depth_ - 1];
        exitApp(exiting);
        stack_[--depth_] = nullptr;
        runtimePolicyChanged();
    }

    hal::input::resetRotaryEncoderPins();
    hal::input::beginRotaryInterrupts();
    hal::input::resyncRotaryQuadrature();

    enterApp(top());
    armInputBlock(120);
}

void NavHandler::prepareForSleep() {
    if (shuttingDown_) return;
    App* app = root();
    if (app && app->onPrepareSleep) app->onPrepareSleep();
    popToRoot();
    usageEnd(top(),"app.sleep");
}

void NavHandler::reenterRoot() {
    if (shuttingDown_) return;
    App* app = root();
    if (!app) return;
    if (app->currentScreen && app->currentScreen->onExit) app->currentScreen->onExit();
    enterApp(app);
}

void NavHandler::replaceRoot(App* rootApp) {
    if (shuttingDown_) return;
    if (!rootApp) return;

    while (depth_ > 0) {
        App* exiting = stack_[depth_ - 1];
        exitApp(exiting);
        stack_[--depth_] = nullptr;
        runtimePolicyChanged();
    }

    stack_[0] = rootApp;
    depth_ = 1;
    runtimePolicyChanged();
    enterApp(rootApp, true);
    armInputBlock(120);
}

void NavHandler::shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    workflowPolicy_ = {};
    // Unlike dismissal, closing the system view must not resume its app.
    if (systemScreen_ && systemScreen_->onExit) systemScreen_->onExit();
    systemScreen_ = nullptr;
    savedAudioGroup_ = nullptr;
    systemInterruptedOptimized_ = false;
    sfx::setActiveAudioGroup(nullptr);
    while (depth_ > 0) {
        exitApp(stack_[depth_ - 1]);
        stack_[--depth_] = nullptr;
        runtimePolicyChanged();
    }
    inputBlockedUntilMs_ = 0;
}

App* NavHandler::top() const {
    return depth_ > 0 ? stack_[depth_ - 1] : nullptr;
}

App* NavHandler::root() const {
    return depth_ > 0 ? stack_[0] : nullptr;
}

void NavHandler::presentSystemScreen(const scene::Screen* screen) {
    if (shuttingDown_) return;
    if (!screen || systemScreen_ == screen) return;
    if (systemScreen_) dismissSystemScreen();

    usageEnd(top(),"app.cover");
    if (top() && top()->onCover) top()->onCover();
    savedAudioGroup_ = sfx::activeAudioGroup();
    sfx::setActiveAudioGroup(nullptr);
    systemInterruptedOptimized_ = hal::display::isOptimizedRender();
    if (systemInterruptedOptimized_) hal::display::exitOptimizedRender();

    systemScreen_ = screen;
    inputBlockedUntilMs_ = 0;
    if (systemScreen_->onEnter) systemScreen_->onEnter();
    hal::display::markAllDirty();
}

void NavHandler::dismissSystemScreen() {
    if (shuttingDown_) return;
    if (!systemScreen_) return;
    if (systemScreen_->onExit) systemScreen_->onExit();
    sfx::setActiveAudioGroup(savedAudioGroup_);
    systemScreen_ = nullptr;
    savedAudioGroup_ = nullptr;

    App* interrupted = top();
    usageStart(interrupted,"app.resume");
    if (interrupted && interrupted->onResume) interrupted->onResume();
    if (interrupted && interrupted->currentScreen && interrupted->currentScreen->onEnter) {
        interrupted->currentScreen->onEnter();
    }
    if (systemInterruptedOptimized_) hal::display::enterOptimizedRender();
    systemInterruptedOptimized_ = false;
    runtimePolicyChanged();
    armInputBlock(120);
    invalidateChrome();
    hal::display::markAllDirty();
}

void NavHandler::dispatch(hal::input::InputEvent ev) {
    ui::focused_label::restart(millis());
    using ET = hal::input::EventType;
    using TK = hal::input::TraceKind;
    struct ScrollTraceScope {
        hal::input::InputEvent ev;
        explicit ScrollTraceScope(hal::input::InputEvent e):ev(e){
            ui::setRotaryScrollContext(ev.rotaryGesture,ev.rotaryUndo);
            if(ev.type==ET::Scroll){hal::input::encoderTraceContext(ev.traceId);hal::input::encoderTrace(TK::Dispatch,ev.traceId,ev.delta);}
        }
        ~ScrollTraceScope(){ui::setRotaryScrollContext(0,false);if(ev.type==ET::Scroll){hal::input::encoderTrace(TK::DispatchEnd,ev.traceId);hal::input::encoderTraceContext(0);}}
        void route(int reason){if(ev.type==ET::Scroll)hal::input::encoderTrace(TK::Route,ev.traceId,reason);}
    } trace(ev);

    if(ev.type==ET::PowerSleep) {
        // Alerts retain their snooze/dismiss controls; do not interrupt an update.
        if(!services::automation::wantsInputCapture() && runtimePolicy().allowPowerSleep) {
            Serial.printf("[power-control] C held %lums -> sleep\n",(unsigned long)core::power_button::kHoldMs);
            services::doBatterySleep();
        }
        return;
    }

    if (!systemScreen_ && ev.type!=ET::Chord_AB && inputBlockedUntilMs_ != 0 &&
        (int32_t)(millis() - inputBlockedUntilMs_) < 0) {trace.route(1);return;}
    using Cue=services::vibration::Cue;
    const bool back=ev.type==ET::ButtonA_Tap || ev.type==ET::ButtonA_Press || ev.type==ET::Chord_AB;
    const Cue fallback=
        (ev.type==ET::ButtonA_Tap || ev.type==ET::Chord_AB) ? Cue::Back :
        (ev.type==ET::ButtonB_Tap || ev.type==ET::ButtonC_Tap) ? Cue::Select : Cue::None;
    struct Feedback {
        Cue fallback;
        ~Feedback(){services::vibration::endInputFeedback(fallback);}
    } feedback{fallback};
    services::vibration::beginInputFeedback(back);

    if (systemScreen_) {
        trace.route(systemScreen_->onInput?4:5);
        if (systemScreen_->onInput) systemScreen_->onInput(ev);
        return;
    }

    if (ev.type == ET::Chord_AB) {
        handleChord();
        return;
    }

    if (inputBlockedUntilMs_ != 0 && (int32_t)(millis() - inputBlockedUntilMs_) < 0) {trace.route(1);return;}

    App* t = top();
    if (!t) {trace.route(2);return;}
    trace.route(t->currentScreen && t->currentScreen->onInput?3:5);
    if (t->currentScreen && t->currentScreen->onInput) t->currentScreen->onInput(ev);

}

void NavHandler::handleChord() {
    ui::focused_label::restart(millis());
    App* t = top();
    if (!t) return;

    if (t->currentScreen && t->currentScreen->onChord && t->currentScreen->onChord()) return;

    if (depth_ > 1) pop();
}

void NavHandler::tick(uint32_t now) {
    if (systemScreen_) {
        if (systemScreen_->onTick) systemScreen_->onTick(now);
        return;
    }

    // Service mounted workflows before advancing the visible view. Covered
    // apps retain their hook; system overlays keep the existing early return.
    for (uint8_t i = 0; i < depth_; ++i) {
        App* app = stack_[i];
        if (app && app->onPreTick) app->onPreTick(now);
    }

    App* t = top();
    if (!t) return;

    if (t->currentScreen && t->currentScreen->onTick) t->currentScreen->onTick(now);
}

void NavHandler::draw(TFT_eSprite& canvas) {
    if (systemScreen_) {
        ui::focused_label::reset();
        beginScreenChrome(systemScreen_);
        if (systemScreen_->onDraw) systemScreen_->onDraw(canvas);
        if (services::automation::wantsInputCapture() &&
            !(systemScreen_->ownsStatusBar && systemScreen_->ownsStatusBar())) {
            chrome_.service(millis());
            chrome_.setBackgroundColor(screenChromeColor(nullptr));
            chrome_.invalidate();
            chrome_.paint(hal::display::tft());
        }
        return;
    }

    App* t = top();
    if (!t) return;

    const AppPresentation presentation = appPresentation(t);
    beginScreenChrome(t->currentScreen);
    if (!hal::display::isOptimizedRender()) {
        ui::focused_label::beginFrame(canvas,t->currentScreen,millis());
        if (!presentation.contentUsesStatusArea) canvas.setViewport(0,kSystemStatusBarH,canvas.width(),canvas.height()-kSystemStatusBarH,false);
        if (t->currentScreen && t->currentScreen->onDraw) t->currentScreen->onDraw(canvas);
        canvas.resetViewport();
        ui::focused_label::finishFrame(canvas);
    } else {
        ui::focused_label::reset();
    }

    if (!presentation.hideAppOverlays) {
        for (uint8_t i = 0; i < depth_; ++i) {
            const auto overlay = appPresentation(stack_[i]).drawOverlay;
            if (overlay) overlay(canvas);
        }
    }

    if (presentation.statusBar == AppPresentation::StatusBar::Hidden) return;

    chrome_.service(millis());
    const uint16_t chromeColor =
        t->currentScreen ? screenChromeColor(t->currentScreen) : 0;
    if (t->currentScreen) chrome_.setBackgroundColor(chromeColor);
    if (takeScreenChromeRepaint()) chrome_.invalidate();

    if (hal::display::isOptimizedRender() &&
        presentation.statusBar == AppPresentation::StatusBar::DirectWhenOptimized) {
        chrome_.paintDirect();
    }
    if (!hal::display::isOptimizedRender()) {
        // Paint the status strip, but do NOT force-invalidate every frame: the
        // chrome's own change detection repaints only when clock/battery/wifi/bg
        // actually change, and enterApp() invalidates once per transition. This
        // keeps idle frames from dirtying the framebuffer (so flush() no-ops),
        // fixing clock flicker and the WiFi-connect freeze. Content clears use
        // clearContentArea(), which preserves the bar region.
        if (presentation.statusBar == AppPresentation::StatusBar::GlyphOverlay) {
            chrome_.paintGlyphOverlay(hal::display::tft(), chromeColor);
        } else {
            chrome_.paint(hal::display::tft());
        }
    }
}

void NavHandler::invalidateChrome() {
    chrome_.invalidate();
}

void NavHandler::armInputBlock(uint32_t delayMs) {
    inputBlockedUntilMs_ = millis() + delayMs;
}

core::health::RuntimePolicy NavHandler::runtimePolicy() const {
    core::health::RuntimePolicy result=workflowPolicy_;
    for (int i=0; i<depth_; ++i) {
        const App* app=stack_[i];
        if (app && app->runtimePolicy)
            result=core::health::combine(result, app->runtimePolicy());
    }
    return result;
}

void NavHandler::setWorkflowPolicy(core::health::RuntimePolicy policy) {
    workflowPolicy_=policy;
    runtimePolicyChanged();
}

void NavHandler::runtimePolicyChanged() {
    core::health::publishBackgroundPolicy(runtimePolicy(), depth_);
}
