/**
 * @file boot_app.cpp
 * @brief Boot app — embedded animation/root handoff and auxiliary catalog views.
 */

#include "apps/boot_app.h"

#include "app.h"
#include <Arduino.h>
#include "apps/agent_app.h"
#include "nav_handler.h"

namespace apps {
namespace boot {

void completeAnimation(uint32_t now, uint32_t startedMs, uint32_t scheduledMs) {
    Serial.printf("[boot-timing] event=handoff_begin millis=%lu elapsed=%lu scheduled_ms=%lu\n",
                  (unsigned long)now, (unsigned long)(now - startedMs),
                  (unsigned long)scheduledMs);
    NavHandler& nav = NavHandler::instance();
    const uint32_t handoffStartUs = micros();
    if (nav.depth() == 1 && nav.root() == &apps::boot::app) {
        nav.replaceRoot(&apps::agent::app);
    } else {
        nav.pop();
    }
    Serial.printf("[boot-timing] event=handoff_return millis=%lu elapsed=%lu handoff_us=%lu root_is_agent=%d\n",
                  (unsigned long)millis(),
                  (unsigned long)(millis() - startedMs),
                  (unsigned long)(micros() - handoffStartUs),
                  nav.root() == &apps::agent::app ? 1 : 0);
}

extern const ::scene::Screen kScreenBoot;
extern const ::scene::Screen kScreenPresents;
extern const ::scene::Screen kScreenEyeMorph;
extern const ::scene::Screen kScreenHandoff;

static const ::scene::Screen* const kScreens[] = {
    &kScreenBoot,
    &kScreenPresents,
    &kScreenEyeMorph,
    &kScreenHandoff,
};
static AppPresentation presentationPolicy() {
    // Boot redraws only its animation region after the first frame. The bar
    // background is static, so transparent repainting would accumulate glyphs.
    return {true, false, AppPresentation::StatusBar::Default};
}
App app = SCENE_APP_CATALOG_PRESENTATION("Boot", "boot", &kScreenBoot, nullptr,
                            "src/apps/screen_boot.cpp", kScreens, presentationPolicy);

}  // namespace boot
}  // namespace apps
