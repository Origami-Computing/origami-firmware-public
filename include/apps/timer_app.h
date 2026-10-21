#pragma once

/**
 * @file timer_app.h
 * @brief Automation-backed countdown timer.
 *
 * The countdown service persists and schedules one active device countdown.
 * This app owns its workflow and views. Leaving the app does not
 * pause a running timer; re-entering reconstructs elapsed/remaining time from
 * the persisted wall-clock start and fire times.
 */

#include <stdint.h>

#include <TB_TFT_eSPI.h>

#include "app.h"
#include "hal/hal_input.h"

namespace apps {
namespace timer {

extern App app;

void enter();
void exitApp();
void tick(uint32_t now);
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);

}  // namespace timer
}  // namespace apps
