#pragma once
/**
 * @file golf_app.h
 * @brief Golf (Wii Sports Resort style): swing the kami like a club. Logic in core/golf, art in ui/golf_render
 * and ui/golf_draw. Spec: docs/superpowers/specs/2026-09-30-golf-design.md.
 */
#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace golf {
extern App app;
void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Dev: `golf status`, `golf hole <course 1-4> <n>`, `golf swing <backDeg> <downDps> <yawDeg> <twistDeg>`,
// `golf club <0-7>`, `golf aim <deg>`, `golf auto`, `golf wind <mph> <dirDeg>`, `golf flip yaw|twist`, `golf next`,
// `golf menu`, `golf practice` (Swing Practice), `golf pswing <backDeg> <downDps> <yawDeg> <twistDeg>` (a practice
// swing: shown, not hit), `golf help [next|close]` (the how-to-play panel; hold A on the device).
void devCommand(const char* args, Print& out);
#endif
}  // namespace golf
}  // namespace apps
