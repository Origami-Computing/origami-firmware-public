#pragma once

#include <stdint.h>
#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace ui {
namespace scene {
class Scene;
}
}

namespace scene {

using BuildSceneFn = void (*)(ui::scene::Scene& scene, int16_t width, int16_t height);

/** Scene screen vtable — per docs/scene_architecture.md */
struct Screen {
    const char* name;
    void (*onEnter)();
    void (*onExit)();
    void (*onTick)(uint32_t now);
    void (*onInput)(hal::input::InputEvent ev);
    bool (*onChord)();
    void (*onDraw)(TFT_eSprite& surface);

    /** Stable tooling identifier and side-effect-free node builder. */
    const char* id;
    BuildSceneFn buildScene;

    /** Fallback chrome color when no scene background has been painted.
     * Null uses the theme default; a pointer to black is an explicit black.
     * The optional pure query is sampled after drawing and takes precedence.
     * Referenced colors must have static lifetime. */
    const uint16_t* statusBarBackground;
    uint16_t (*statusBarBackgroundNow)();
    // Pure UI-task query: system screens may supply their own status strip.
    // Ordinary app privileges remain declared through AppPresentation.
    // Aggregate descriptors omit this field to value-initialize it to null.
    bool (*ownsStatusBar)();
};

enum class ScreenTransition : uint8_t { Automatic, Forward, Back, Silent };
void switchTo(App* app, const Screen* screen);
void switchTo(App* app, const Screen* screen, ScreenTransition direction);

}  // namespace scene
