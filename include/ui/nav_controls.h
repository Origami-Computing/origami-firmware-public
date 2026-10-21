#pragma once

#include "hal/hal_input.h"

namespace ui {
namespace nav {

inline bool isBackTap(hal::input::InputEvent ev) {
    return ev.type == hal::input::EventType::ButtonA_Tap;
}

inline bool isBackPress(hal::input::InputEvent ev) {
    return ev.type == hal::input::EventType::ButtonA_Press;
}

inline bool isEnterTap(hal::input::InputEvent ev) {
    return ev.type == hal::input::EventType::ButtonC_Tap;
}

inline bool isEnterPress(hal::input::InputEvent ev) {
    return ev.type == hal::input::EventType::ButtonC_Press;
}

// Compatibility aliases for code that has not migrated to the LB/encoder
// vocabulary yet. New navigation UI should use isEnter*().
inline bool isSelectTap(hal::input::InputEvent ev) {
    return isEnterTap(ev);
}

inline bool isSelectPress(hal::input::InputEvent ev) {
    return isEnterPress(ev);
}

}  // namespace nav
}  // namespace ui
