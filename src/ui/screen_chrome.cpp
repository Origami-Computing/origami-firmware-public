#include "ui/screen_chrome.h"

#include "scene/screen.h"
#include "config/ui_theme.h"

namespace {
const void* s_screen = nullptr;
bool s_hasSceneBackground = false;
uint16_t s_sceneBackground = kUiBg;
bool s_repaintedBar = false;

}  // namespace

void beginScreenChrome(const void* screen) {
    if (s_screen == screen) return;
    s_screen = screen;
    s_hasSceneBackground = false;
    s_repaintedBar = false;
}

void setSceneChromeBackground(uint16_t color, bool repaintedBar) {
    s_sceneBackground = color;
    s_hasSceneBackground = true;
    s_repaintedBar |= repaintedBar;
}

bool takeScreenChromeRepaint() {
    const bool repaint = s_repaintedBar;
    s_repaintedBar = false;
    return repaint;
}

uint16_t screenChromeColor(const scene::Screen* screen) {
    if (s_hasSceneBackground) return s_sceneBackground;
    if (!screen) return kUiBarBg;
    if (screen->statusBarBackgroundNow) return screen->statusBarBackgroundNow();
    return screen->statusBarBackground ? *screen->statusBarBackground : kUiBarBg;
}
