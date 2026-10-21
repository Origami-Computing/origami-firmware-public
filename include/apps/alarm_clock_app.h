#pragma once

/**
 * @file alarm_clock_app.h
 * @brief Multi-alarm clock that integrates with the automation pipeline.
 *
 * The alarms service stores up to eight alarms in NVS and injects enabled alarms into
 * services::automation with stable private server IDs so they survive restarts
 * and remain distinct from cloud-pushed automations.
 *
 * The app opens on the alarm list, with a dedicated Morning Alarm first.
 * Morning Alarm has its own NVS settings (initially off at 07:00), repeats
 * daily, and offers an RB preview in its editor without saving changes.
 *
 *   Alarm list:
 *     Rotary — select an alarm
 *     Encoder press — open the selected alarm or Add Alarm
 *
 *   Alarm detail:
 *     Rotary — move focus between fields; adjust value while editing
 *     Encoder press — toggle field editing, or activate Save
 *     Focus is solid inverted; editing inverts every 500 ms.
 *     LB            — cancel and return to the list
 *     RB            — preview Morning Alarm (in its editor only)
 *
 * Enabled rows show a green dot. The alarms service keeps every
 * enabled alarm present in the automation queue after clock sync or firing.
 */

#include <stdint.h>
#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace alarm_clock {

/** The App descriptor — pass to NavHandler::push() to open the alarm clock. */
extern App app;

void enter();
void exitApp();
void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);

}  // namespace alarm_clock
}  // namespace apps
