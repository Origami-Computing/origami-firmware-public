#pragma once
#include "ui/proposals/design_system.h"

namespace ui { namespace onboarding_flow {
template<class Canvas>
void exitPrompt(Canvas& canvas, bool exitSelected) {
    pixel_dialogue::draw(canvas, {12, 86, 256, 132}, 0, 0xffff);
    design::center(canvas, "Do you want to exit", 105);
    design::center(canvas, "this tutorial?", 129);
    design::choice(canvas, "Yes, Exit", 24, 163, 110, 36, exitSelected);
    design::choice(canvas, "No, Resume", 146, 163, 110, 36, !exitSelected);
}
}}
