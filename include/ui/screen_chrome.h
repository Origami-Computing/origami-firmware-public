#pragma once
#include <stddef.h>

#include <stdint.h>

namespace scene { struct Screen; }
uint16_t screenChromeColor(const scene::Screen* screen);

// Scope the last painted background to a screen; retain it on idle frames.
void beginScreenChrome(const void* screen);
// Called by full-content clears and scene background rectangles, not text/panels.
void setSceneChromeBackground(uint16_t color, bool repaintedBar = false);
bool takeScreenChromeRepaint();
