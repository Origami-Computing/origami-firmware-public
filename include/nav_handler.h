#pragma once

/**
 * @file nav_handler.h
 * @brief Singleton app-stack navigator for Origami firmware.
 *
 * NavHandler owns the lifecycle of every App on the device.  It maintains a
 * fixed-depth stack of App pointers and handles:
 *
 *   - Scene entry and exit during push / pop
 *   - Chord (LB+RB) routing: asks the active screen first, pops if refused
 *   - Input debounce guard after every pop() to eat stray releases
 *   - System chrome (status bar) painting on every frame
 *   - Mic drain before any SPI stall to prevent audio underruns
 *
 * Navigation semantics
 * --------------------
 * The stack always has the Kami home-screen as element 0 (the root).  All
 * other apps are pushed on top.  LB+RB chord unwinds one level at a time:
 *
 *   [Kami]                chord -> Eyes.onChord -> true  (stays)
 *   [Kami] rotary         -> Kami pushes Menu   -> [Kami, Menu]
 *   [Kami, Menu]          chord -> Menu screen refuses -> NavHandler pops -> [Kami]
 *   [Kami, Menu] press encoder -> Menu pushes Camera -> [Kami, Menu, Camera]
 *   [Kami, Menu, Camera]  chord -> Camera screen refuses -> pops -> [Kami, Menu]
 *
 * Direct-panel screens set onDraw = nullptr and use the existing optimized
 * renderer/display HAL path. Apps do not gain raw driver/session ownership. NavHandler skips scene drawing while optimized rendering is active.
 */

#include <stdint.h>
#include "app.h"
#include "hal/hal_input.h"
#include "ui/system_status_bar.h"
#include <TB_TFT_eSPI.h>

namespace sfx {
struct AudioGroup;
}

class NavHandler {
public:
    /** Return the process-wide singleton. */
    static NavHandler& instance();

    /** Boot initialisation: set root app and enter its initial screen.
     *  Call before push/pop/dispatch; may start a fresh lifecycle after shutdown(). */
    void  init(App* root);

    /** Mount a new app on top of the stack. */
    void  push(App* app);
    // Open one existing instance: uncover it if mounted, otherwise push.
    bool open(App* app);

    /** Unmount the top app and re-enter the app below. */
    void  pop();

    /** Pop all apps until only the root remains, then re-enter root once. */
    void  popToRoot();

    /** Prepare the retained root workflow for sleep, then close covering apps
     * through popToRoot(). Preserves the existing root re-entry behavior.
     * UI-task only; the power service owns the subsequent sleep/wake sequence. */
    void prepareForSleep();

    /** Re-enter the root initial screen without changing stack depth. */
    void  reenterRoot();

    /** Terminal UI-task teardown: close system view and all apps, top to root.
     * Never resume/re-enter views. Idempotent; init() may start a fresh lifecycle. */
    void shutdown();

    /** Replace the entire stack with a new root app.
     *  Used by early boot so the lightweight Boot app can render before the
     *  heavier Kami root performs SD-backed scene and transport setup. */
    void  replaceRoot(App* root);

    /** The app currently receiving tick / input / draw calls. */
    App*  top()   const;

    /** The root app (always at stack index 0; typically Kami). */
    App*  root()  const;

    /** Present/dismiss a system-owned Screen above the app stack. */
    void presentSystemScreen(const scene::Screen* screen);
    void dismissSystemScreen();
    bool systemScreenActive() const { return systemScreen_ != nullptr; }

    /** Current stack depth (1 = only root, 2 = one app on top of root, …). */
    int   depth() const { return depth_; }

    /** Intersect permissions from every mounted app, including covered apps. */
    core::health::RuntimePolicy runtimePolicy() const;
    // UI task: publish dynamic app constraints immediately after changing them,
    // before blocking work or releasing a service reservation. Lifecycle/stack
    // transitions also refresh automatically; never invoke from a worker.
    void runtimePolicyChanged();

    // A workflow may temporarily hand the visible stack to another app (e.g.
    // onboarding's live voice practice). Its owner sets constraints here and
    // clears them on return/cancel. UI-task only; one such handoff at a time.
    void setWorkflowPolicy(core::health::RuntimePolicy policy);

    /** Route an input event.
     *  Chord_AB is intercepted here and delivered to handleChord(); all other
     *  event types are forwarded to the active screen unless the input block
     *  guard is active (set by armInputBlock() after init/push/pop). */
    void  dispatch(hal::input::InputEvent ev);

    /** Per-frame tick.
     *  Drains pending mic samples first - this must happen before any SPI
     *  transaction (SD write, I²C, TFT flush) or the I2S DMA ring overflows
     *  and produces an audible gap. Then ticks the active screen. */
    void  tick(uint32_t now);

    /** Per-frame draw.
     *  Calls the active screen's onDraw(canvas) when non-null and the canvas
     *  pipeline is active, then paints the system chrome (status bar).
     *  Direct-panel apps skip the onDraw call but still get chrome. */
    void  draw(TFT_eSprite& canvas);

    /** Mark the chrome as needing a full repaint next frame. */
    void  invalidateChrome();

    /** Block non-chord input until now+delayMs.
     *  Called automatically by init() (400 ms) and push/pop (120 ms) to absorb
     *  the button release that triggered the navigation action. */
    void  armInputBlock(uint32_t delayMs);

    /** Direct access to the system status bar for boot-time priming. */
    ui::SystemStatusBar& chrome() { return chrome_; }

private:
    NavHandler() = default;

    /** Deliver a chord event: ask the active screen, pop if refused. */
    void  handleChord();

    // Stack depth is fixed at compile time to avoid heap fragmentation.
    static constexpr int kMaxDepth = 8;
    App*     stack_[kMaxDepth] = {};
    int      depth_ = 0;
    bool shuttingDown_ = false;
    core::health::RuntimePolicy workflowPolicy_;
    const scene::Screen* systemScreen_ = nullptr;
    sfx::AudioGroup* savedAudioGroup_ = nullptr;
    bool systemInterruptedOptimized_ = false;

    ui::SystemStatusBar chrome_;

    // Absolute millis() timestamp before which non-chord events are dropped.
    // Zero means the guard is inactive.
    uint32_t inputBlockedUntilMs_ = 0;
};
