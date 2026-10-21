#pragma once

/**
 * @file tetris_app.h
 * @brief Standard Tetris clone rendered entirely on the canvas sprite.
 *
 * The board is 10×20 cells, each cell 11 px with a 1 px gap.  Pieces are
 * stored as 4×2 int8_t arrays of (row, col) offsets in all four rotations.
 * A ghost piece is drawn at the hard-drop destination in a dimmed colour.
 *
 * Input map:
 *   Rotary    — move piece left / right (accumulated, fires on ±2 detents)
 *   A press   — rotate piece clockwise (wall-kick: tries px±1 if blocked)
 *   B press   — hard drop; on game-over screen: returns to menu
 *   A+B chord — onChord nullptr → NavHandler pops back to menu at any time
 *
 * SFX events: move, rotate, lock, line clear (1 or 4), level-up, game-over.
 * The SFX codec is brought up lazily on first sfx::play() and torn down by
 * sfx::service() ~2 s after the last sound — no init/deinit needed here.
 */

#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace tetris {

/** The App descriptor — pass to NavHandler::push() to start a game. */
extern App app;

void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);

}  // namespace tetris
}  // namespace apps
