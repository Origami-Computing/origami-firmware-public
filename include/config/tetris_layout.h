#pragma once

#include <stdint.h>
#include "ui/safe_area_layout.h"

namespace tetris_layout {

// BEGIN apply_ui_source_edits.py tetris/game
static constexpr int16_t kBoardCols = 10;
static constexpr int16_t kBoardRows = 20;
static constexpr int16_t kCellSize = 9;
static constexpr int16_t kCellGap = 1;
static constexpr int16_t kBoardX = 24;
static constexpr int16_t kBoardY = ui::safe_area::contentTop()+7;
static constexpr int16_t kBoardW = kBoardCols*12;
static constexpr int16_t kBoardH = kBoardRows*kCellSize;
static constexpr int16_t kScoreX = 166;
static constexpr int16_t kScoreY = kBoardY;
static constexpr int16_t kScoreW = 96;
static constexpr int16_t kScoreH = 18;
static constexpr int16_t kLevelX = 166;
static constexpr int16_t kLevelY = kScoreY+26;
static constexpr int16_t kLevelW = 90;
static constexpr int16_t kLevelH = 14;
static constexpr int16_t kLinesX = 166;
static constexpr int16_t kLinesY = kLevelY+22;
static constexpr int16_t kLinesW = 90;
static constexpr int16_t kLinesH = 12;
static constexpr int16_t kNextX = 185;
static constexpr int16_t kNextY = kLinesY+40;
static constexpr int16_t kNextCell = 7;
static constexpr int16_t kNextBlock = 6;
static constexpr int16_t kGameOverX = kBoardX;
static constexpr int16_t kGameOverY = 92;
static constexpr int16_t kGameOverW = kBoardW;
static constexpr int16_t kGameOverH = 22;
// END apply_ui_source_edits.py tetris/game
static_assert(ui::safe_area::fits({kBoardX-1,kBoardY-1,kBoardW+2,kBoardH+2}),
              "Board, border and cell grid must fit as one group");

}  // namespace tetris_layout
