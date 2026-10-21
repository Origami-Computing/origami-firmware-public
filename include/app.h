#pragma once

/**
 * @file app.h
 * @brief App struct and lifecycle contract for Origami firmware.
 *
 * Every app is a scene app. NavHandler owns the active screen lifecycle and
 * dispatches input, tick, chord, and draw callbacks to `currentScreen`.
 */

#include <stdint.h>
#include "core/health_policy.h"

class TFT_eSprite;

namespace scene {
struct Screen;
}

/** Explicit presentation privileges. Ordinary apps use the zero/default policy.
 * This selects existing rendering paths; it does not create a drawing surface. */
struct AppPresentation {
    enum class StatusBar {
        Default,             // Framebuffer chrome; optimized screens paint their own.
        Hidden,              // The view supplies its own chrome or uses the whole panel.
        DirectWhenOptimized, // Paint the existing bar through the direct-panel path.
        GlyphOverlay         // Preserve the view background beneath status glyphs.
    };
    // Safe even with plain default construction (AppPresentation p;). Every
    // exceptional privilege must be selected explicitly by the app.
    constexpr AppPresentation(bool usesStatusArea = false, bool hidesOverlays = false,
                              StatusBar bar = StatusBar::Default,
                              void (*overlay)(TFT_eSprite&) = nullptr)
        : contentUsesStatusArea(usesStatusArea), hideAppOverlays(hidesOverlays),
          statusBar(bar), drawOverlay(overlay) {}

    bool contentUsesStatusArea;
    bool hideAppOverlays;
    StatusBar statusBar;
    /** Optional view overlay, painted for mounted apps root-to-top after the
     * visible view and before system chrome. The top app can suppress overlays;
     * system screens always suppress them. Must not navigate or alter policy.
     * Renderer retains responsibility for framebuffer versus direct painting. */
    void (*drawOverlay)(TFT_eSprite&);
};

struct App {
    const char* name;

    /** Immutable entry screen; NavHandler resets currentScreen on app entry. */
    const scene::Screen* initialScreen;
    const scene::Screen* currentScreen;

    /** Optional UI-task workflow service hook, root-to-top for every mounted app
     * before the visible screen ticks. Runs while covered by another app, but
     * not under a system screen. Must be bounded and must not navigate.
     * Use services for hardware work; this is not a background worker. */
    void (*onPreTick)(uint32_t now);

    /** App-level teardown on pop (nullable). Screen.onExit runs first. */
    void (*onExit)();

    /** Stable tooling identifier and source location. */
    const char* id;
    const char* source;

    /** Complete screen catalog used by navigation diagnostics and editor tools. */
    const scene::Screen* const* screens;
    uint8_t screenCount;

    /** Optional side-effect-free policy query; may reflect the app's current view. */
    AppPresentation (*presentation)();

    /** Optional pure query. Constraints apply even while this app is covered. */
    core::health::RuntimePolicy (*runtimePolicy)();

    /** Once per stack activation, before the initial screen enters. Not called
     * when uncovered, after system overlays, or during screen switches. */
    void (*onEnter)();

    /** Workflow recovery before view re-entry after being uncovered, a system
     * overlay, or root refresh. Not called for activation or screen switches. */
    void (*onResume)();

    /** Optional root-only sleep preparation, before covering apps are removed.
     * The root remains mounted; other apps receive normal onExit instead.
     * May cancel workflow operations through services. Must not navigate.
     * Wake refreshes the root view through the existing resume path. */
    void (*onPrepareSleep)();

    // Before another app or system view covers this app. Release view-scoped
    // resources here; onResume restores workflow before the view re-enters.
    void (*onCover)();
};

/** App descriptor; `INIT` is the initial scene::Screen*. */
#define SCENE_APP(NAME, INIT, EXIT) \
    { (NAME), (INIT), nullptr, nullptr, (EXIT), nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr }

#define SCENE_APP_CATALOG(NAME, ID, INIT, EXIT, SOURCE, SCREENS) \
    { (NAME), (INIT), nullptr, nullptr, (EXIT), (ID), (SOURCE), (SCREENS), \
      (uint8_t)(sizeof(SCREENS) / sizeof((SCREENS)[0])), nullptr, nullptr, nullptr, nullptr, nullptr }

/** Exceptional presentation is visible at the app descriptor, not in navigation. */
#define SCENE_APP_CATALOG_PRESENTATION(NAME, ID, INIT, EXIT, SOURCE, SCREENS, POLICY) \
    { (NAME), (INIT), nullptr, nullptr, (EXIT), (ID), (SOURCE), (SCREENS), \
      (uint8_t)(sizeof(SCREENS) / sizeof((SCREENS)[0])), (POLICY), nullptr, nullptr, nullptr, nullptr }

inline AppPresentation appPresentation(const App* app) {
    return app && app->presentation ? app->presentation() : AppPresentation{};
}

/** Declare app activation alongside explicit presentation/runtime policies. */
#define SCENE_APP_CATALOG_LIFECYCLE_HOOKS(NAME, ID, INIT, ENTER, RESUME, EXIT, SOURCE, SCREENS, PRESENTATION, RUNTIME) \
    { (NAME), (INIT), nullptr, nullptr, (EXIT), (ID), (SOURCE), (SCREENS), \
      (uint8_t)(sizeof(SCREENS) / sizeof((SCREENS)[0])), (PRESENTATION), (RUNTIME), (ENTER), (RESUME), nullptr }

#define SCENE_APP_CATALOG_LIFECYCLE_POLICIES(NAME, ID, INIT, ENTER, EXIT, SOURCE, SCREENS, PRESENTATION, RUNTIME) \
    SCENE_APP_CATALOG_LIFECYCLE_HOOKS(NAME, ID, INIT, ENTER, nullptr, EXIT, SOURCE, SCREENS, PRESENTATION, RUNTIME)

/** Declare optional presentation and runtime policies without navigation exceptions. */
#define SCENE_APP_CATALOG_POLICIES(NAME, ID, INIT, EXIT, SOURCE, SCREENS, PRESENTATION, RUNTIME) \
    SCENE_APP_CATALOG_LIFECYCLE_POLICIES(NAME, ID, INIT, nullptr, EXIT, SOURCE, SCREENS, PRESENTATION, RUNTIME)

/** App-owned session setup/teardown; screen callbacks own view refresh. */
#define SCENE_APP_CATALOG_LIFECYCLE(NAME, ID, INIT, ENTER, EXIT, SOURCE, SCREENS) \
    SCENE_APP_CATALOG_LIFECYCLE_POLICIES(NAME, ID, INIT, ENTER, EXIT, SOURCE, SCREENS, nullptr, nullptr)
