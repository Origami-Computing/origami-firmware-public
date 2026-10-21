#include "config/screen_chrome_layout.h"
/**
 * @file tetris_app.cpp
 * @brief Tetris — single-file implementation with custom pixel-font score display.
 *
 * Piece encoding
 * --------------
 * Each of the 7 tetrominos has 4 rotations stored as kPieces[piece][rotation][cell].
 * Each cell is a {row_offset, col_offset} pair relative to the pivot point {px, py}.
 * This encoding is compact and avoids per-rotation bitmask shifting.
 *
 * Input mapping
 * -------------
 *   Rotary  → accumulate into s.rotaryAccum; move left/right when |accum| ≥ 2
 *             (threshold of 2 prevents jitter from half-clicks on the encoder)
 *   Button A → rotate CW with simple wall-kick: try in-place, then px+1, then px-1
 *   Button B → hard drop (instant gravity to the lowest valid row) + lock
 *   Button A (game over) → restart
 *   Button B (game over) → pop app via NavHandler
 *
 * Wall-kick
 * ---------
 * When rotation is blocked by a wall or locked cell, one-cell nudges left or right
 * are attempted.  The I-piece occasionally still fails near corners — a full SRS
 * kick table is omitted to keep the code small.
 *
 * Ghost piece
 * -----------
 * Drawn in kGrid colour at the lowest position where the piece would land.  Skipped
 * when the ghost and the active piece overlap (piece is already on the floor) to avoid
 * painting over the active piece.
 *
 * Scoring (original NES-style)
 * ----------------------------
 *   1-line: 100 × (level+1)
 *   2-line: 300 × (level+1)
 *   3-line: 500 × (level+1)
 *   4-line: 800 × (level+1)
 *   Level advances every 10 lines; speed table tops out at level 19.
 *
 * Rendering
 * ---------
 * render() paints to the canvas sprite and calls markAllDirty() — the normal
 * canvas pipeline flush picks it up next frame.  The custom 5×7 pixel font
 * (kDigitFont) is used for score/level/lines so they're readable at the small
 * panel size without loading a full TFT_eSPI font.
 */

#ifdef MENU_APP

#include "apps/tetris_app.h"
#include "config/tetris_layout.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "sfx/sfx_lib.h"
#include "config/ui_layout.h"
#include "ui/scene.h"
#include "ui/screen_chrome.h"
#include "app.h"
#include <Arduino.h>
#include <string.h>



namespace apps {
namespace tetris {

static constexpr int kCols   = tetris_layout::kBoardCols;
static constexpr int kRows   = tetris_layout::kBoardRows;
static constexpr int kCell   = tetris_layout::kCellSize;
static constexpr int kGap    = tetris_layout::kCellGap;
static constexpr int kBoardW = tetris_layout::kBoardW;
static constexpr int kBoardH = tetris_layout::kBoardH;
static constexpr int kCellW = kBoardW / kCols;
static constexpr int kBoardX = tetris_layout::kBoardX;
static constexpr int kBoardY = tetris_layout::kBoardY;

static const uint16_t kColors[] = {
  0x0000, 0x07FF, 0x001F, 0xFD20, 0xFFE0, 0x07E0, 0xF81F, 0xF800,
};
static constexpr uint16_t kBg       = 0x10A2;
static constexpr uint16_t kGrid     = 0x18E3;
static constexpr uint16_t kScreenBg = 0x0000;
static uint16_t s_boardTiles[kRows * kCols];

struct Piece { int8_t cells[4][2]; };

static const Piece kPieces[7][4] = {
  // I
  {{{ {0,-1},{0,0},{0,1},{0,2} }}, {{ {-1,0},{0,0},{1,0},{2,0} }},
   {{ {0,-1},{0,0},{0,1},{0,2} }}, {{ {-1,0},{0,0},{1,0},{2,0} }}},
  // J
  {{{ {-1,-1},{0,-1},{0,0},{0,1} }}, {{ {-1,0},{-1,1},{0,0},{1,0} }},
   {{ {0,-1},{0,0},{0,1},{1,1} }},  {{ {-1,0},{0,0},{1,0},{1,-1} }}},
  // L
  {{{ {0,-1},{0,0},{0,1},{1,-1} }},  {{ {-1,0},{0,0},{1,0},{-1,-1} }},
   {{ {0,-1},{0,0},{0,1},{-1,1} }},  {{ {-1,0},{0,0},{1,0},{1,1} }}},
  // O
  {{{ {0,0},{1,0},{0,1},{1,1} }}, {{ {0,0},{1,0},{0,1},{1,1} }},
   {{ {0,0},{1,0},{0,1},{1,1} }}, {{ {0,0},{1,0},{0,1},{1,1} }}},
  // S
  {{{ {0,0},{1,0},{-1,1},{0,1} }},  {{ {0,-1},{0,0},{1,0},{1,1} }},
   {{ {0,0},{1,0},{-1,1},{0,1} }},  {{ {0,-1},{0,0},{1,0},{1,1} }}},
  // T
  {{{ {-1,0},{0,0},{1,0},{0,1} }},  {{ {0,-1},{0,0},{0,1},{-1,0} }},
   {{ {-1,0},{0,0},{1,0},{0,-1} }}, {{ {0,-1},{0,0},{0,1},{1,0} }}},
  // Z
  {{{ {-1,0},{0,0},{0,1},{1,1} }},  {{ {0,0},{0,1},{1,-1},{1,0} }},
   {{ {-1,0},{0,0},{0,1},{1,1} }},  {{ {0,0},{0,1},{1,-1},{1,0} }}},
};

struct State {
  uint8_t board[kRows][kCols];
  int piece, rot, px, py;
  int nextPiece;
  int score, lines, level;
  uint32_t lastDropMs;
  bool gameOver;
  int rotaryAccum;
};
static State s;

static void syncBoardTiles() {
  for (int r = 0; r < kRows; r++) {
    for (int c = 0; c < kCols; c++) {
      const uint8_t val = s.board[r][c];
      s_boardTiles[r * kCols + c] = val > 0 ? kColors[val] : kBg;
    }
  }
}

// Drop interval in ms: roughly mirrors the NES Tetris speed table.
// Capped at index 19 — the game never speeds up past 50 ms per row.
static int dropInterval() {
  const int speeds[] = {800,720,630,550,470,380,300,220,140,100,80,80,80,70,70,70,60,60,60,50};
  int lvl = s.level;
  if (lvl >= 20) lvl = 19;
  return speeds[lvl];
}

static bool pieceFits(int piece, int rot, int px, int py) {
  const Piece& p = kPieces[piece][rot & 3];
  for (int i = 0; i < 4; i++) {
    int c = px + p.cells[i][0];
    int r = py + p.cells[i][1];
    if (c < 0 || c >= kCols || r >= kRows) return false;
    if (r >= 0 && s.board[r][c] != 0) return false;
  }
  return true;
}

static void lockPiece() {
  const Piece& p = kPieces[s.piece][s.rot & 3];
  uint8_t color = (uint8_t)(s.piece + 1);
  for (int i = 0; i < 4; i++) {
    int c = s.px + p.cells[i][0];
    int r = s.py + p.cells[i][1];
    if (r >= 0 && r < kRows && c >= 0 && c < kCols)
      s.board[r][c] = color;
  }
}

static int clearLines() {
  int cleared = 0;
  for (int r = kRows - 1; r >= 0; r--) {
    bool full = true;
    for (int c = 0; c < kCols; c++) {
      if (s.board[r][c] == 0) { full = false; break; }
    }
    if (full) {
      for (int rr = r; rr > 0; rr--)
        memcpy(s.board[rr], s.board[rr-1], kCols);
      memset(s.board[0], 0, kCols);
      cleared++;
      r++;
    }
  }
  return cleared;
}

static void spawnPiece() {
  s.piece = s.nextPiece;
  s.nextPiece = esp_random() % 7;
  s.rot = 0;
  s.px = kCols / 2;
  s.py = 0;
  if (!pieceFits(s.piece, s.rot, s.px, s.py)) {
    s.gameOver = true;
  }
}

static void scoreLines(int cl, int prevLevel) {
  if (cl > 0) {
    const int pts[] = {0, 100, 300, 500, 800};
    s.score += pts[cl] * (s.level + 1);
    s.lines += cl;
    s.level = s.lines / 10;
    if (cl >= 4) sfx::play(sfx::kSfxTetrisLine4);
    else         sfx::play(sfx::kSfxTetrisLine1);
  }
  if (s.level > prevLevel) sfx::play(sfx::kSfxTetrisLevelUp);
}

static void buildTetrisScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::setCurrentScene(&scene);
  syncBoardTiles();

  scene.addRect("tetris.bg", {0, 0, width, height}, kScreenBg, 0, 0);
  scene.addRect("tetris.board.border", {(int16_t)(kBoardX - 1), (int16_t)(kBoardY - 1),
                                        (int16_t)(kBoardW + 2), (int16_t)(kBoardH + 2)},
                kGrid, 0, 1);
  scene.addRect("tetris.board.bg", {kBoardX, kBoardY, kBoardW, kBoardH}, kBg, 0, 1);
  scene.addTileGrid("tetris.board.tiles", {kBoardX, kBoardY, kBoardW, kBoardH},
                    s_boardTiles, kCols, kRows, kGap, kBg, false, 2);

  char score[16];
  char level[16];
  char lines[16];
  snprintf(score, sizeof(score), "%d", s.score);
  snprintf(level, sizeof(level), "L %d", s.level);
  snprintf(lines, sizeof(lines), "%d ln", s.lines);
  scene.addLabel("tetris.score",
                 {tetris_layout::kScoreX, tetris_layout::kScoreY,
                  tetris_layout::kScoreW, tetris_layout::kScoreH},
                 score, TFT_WHITE, kScreenBg, 2, ui::scene::TextAlign::TopLeft, 2);
  scene.addLabel("tetris.level",
                 {tetris_layout::kLevelX, tetris_layout::kLevelY,
                  tetris_layout::kLevelW, tetris_layout::kLevelH},
                 level, 0x07E0, kScreenBg, 1, ui::scene::TextAlign::TopLeft, 2);
  scene.addLabel("tetris.lines",
                 {tetris_layout::kLinesX, tetris_layout::kLinesY,
                  tetris_layout::kLinesW, tetris_layout::kLinesH},
                 lines, 0x8410, kScreenBg, 1, ui::scene::TextAlign::TopLeft, 2);

  if (!s.gameOver) {
    const Piece& p = kPieces[s.piece][s.rot & 3];
    int ghostY = s.py;
    while (pieceFits(s.piece, s.rot, s.px, ghostY + 1)) ghostY++;
    for (int i = 0; i < 4; i++) {
      int gc = s.px + p.cells[i][0];
      int gr = ghostY + p.cells[i][1];
      if (gr >= 0 && gr < kRows && gc >= 0 && gc < kCols) {
        char id[24];
        snprintf(id, sizeof(id), "tetris.ghost.%d", i);
        scene.addRect(id, {(int16_t)(kBoardX + gc * kCellW + kGap),
                           (int16_t)(kBoardY + gr * kCell + kGap),
                           (int16_t)(kCellW - kGap * 2), (int16_t)(kCell - kGap * 2)},
                      kGrid, 0, 2);
      }
      int cc = s.px + p.cells[i][0];
      int cr = s.py + p.cells[i][1];
      if (cr >= 0 && cr < kRows && cc >= 0 && cc < kCols) {
        char id[24];
        snprintf(id, sizeof(id), "tetris.piece.%d", i);
        scene.addRect(id, {(int16_t)(kBoardX + cc * kCellW + kGap),
                           (int16_t)(kBoardY + cr * kCell + kGap),
                           (int16_t)(kCellW - kGap * 2), (int16_t)(kCell - kGap * 2)},
                      kColors[s.piece + 1], 0, 3);
      }
    }
  }

  const Piece& np = kPieces[s.nextPiece][0];
  for (int i = 0; i < 4; i++) {
    char id[24];
    snprintf(id, sizeof(id), "tetris.next.%d", i);
    scene.addRect(id, {(int16_t)(tetris_layout::kNextX + np.cells[i][0] * tetris_layout::kNextCell),
                       (int16_t)(tetris_layout::kNextY + np.cells[i][1] * tetris_layout::kNextCell),
                       tetris_layout::kNextBlock, tetris_layout::kNextBlock},
                  kColors[s.nextPiece + 1], 0, 2);
  }

  if (s.gameOver) {
    scene.addLabel("tetris.game_over",
                   {tetris_layout::kGameOverX, tetris_layout::kGameOverY,
                    tetris_layout::kGameOverW, tetris_layout::kGameOverH},
                   "GAME OVER", 0xF800, kBg, 1, ui::scene::TextAlign::MiddleCenter, 4);
  }
}

static void render(TFT_eSPI& d) {
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildTetrisScene(scene, d.width(), d.height());
  ui::scene::renderCurrentScene(d);
  hal::display::markAllDirty();
}

static void prepareWorkflow() {
  memset(s.board, 0, sizeof(s.board));
  s.score = 0;
  s.lines = 0;
  s.level = 0;
  s.gameOver = false;
  s.lastDropMs = millis();
  s.rotaryAccum = 0;
  s.nextPiece = esp_random() % 7;
  spawnPiece();

  // sfx is lazy: codec / I2S TX is brought up on first sfx::play() and torn down
  // by sfx::service() ~2 s after the last sound — no init/deinit hook needed here.

  Serial.println("[tetris] enter");
}

static void exitApp() {
  Serial.println("[tetris] exit");
}

void tick() {
  if (s.gameOver) return;

  uint32_t now = millis();
  int prevLevel = s.level;

  if (s.rotaryAccum >= 2) {
    if (pieceFits(s.piece, s.rot, s.px + 1, s.py)) {
      s.px++;
      sfx::play(sfx::kSfxTetrisMove);
    }
    s.rotaryAccum = 0;
  } else if (s.rotaryAccum <= -2) {
    if (pieceFits(s.piece, s.rot, s.px - 1, s.py)) {
      s.px--;
      sfx::play(sfx::kSfxTetrisMove);
    }
    s.rotaryAccum = 0;
  }

  if (now - s.lastDropMs >= (uint32_t)dropInterval()) {
    if (pieceFits(s.piece, s.rot, s.px, s.py + 1)) {
      s.py++;
    } else {
      lockPiece();
      sfx::play(sfx::kSfxTetrisLock);
      int cl = clearLines();
      scoreLines(cl, prevLevel);
      spawnPiece();
      if (s.gameOver) sfx::play(sfx::kSfxTetrisGameOver);
    }
    s.lastDropMs = now;
  }
}

void onDraw(TFT_eSprite& canvas) {
  render(canvas);
  setSceneChromeBackground(kScreenBg);
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;

  if (ev.type == ET::ButtonA_Press) {
    if (s.gameOver) {
      memset(s.board, 0, sizeof(s.board));
      s.score = 0;
      s.lines = 0;
      s.level = 0;
      s.gameOver = false;
      s.lastDropMs = millis();
      s.rotaryAccum = 0;
      s.nextPiece = esp_random() % 7;
      spawnPiece();
      return;
    }
    int newRot = (s.rot + 1) & 3;
    bool rotated = false;
    if (pieceFits(s.piece, newRot, s.px, s.py))
      { s.rot = newRot; rotated = true; }
    else if (pieceFits(s.piece, newRot, s.px + 1, s.py))
      { s.px++; s.rot = newRot; rotated = true; }
    else if (pieceFits(s.piece, newRot, s.px - 1, s.py))
      { s.px--; s.rot = newRot; rotated = true; }
    if (rotated) sfx::play(sfx::kSfxTetrisRotate);
    return;
  }

  if (ev.type == ET::ButtonB_Press) {
    if (s.gameOver) { NavHandler::instance().pop(); return; }
    int prevLevel = s.level;
    while (pieceFits(s.piece, s.rot, s.px, s.py + 1)) s.py++;
    lockPiece();
    int cl = clearLines();
    if (cl > 0) scoreLines(cl, prevLevel);
    else sfx::play(sfx::kSfxTetrisDrop);
    if (s.level > prevLevel) sfx::play(sfx::kSfxTetrisLevelUp);
    spawnPiece();
    if (s.gameOver) sfx::play(sfx::kSfxTetrisGameOver);
    s.lastDropMs = millis();
    return;
  }

  if (ev.type == ET::Scroll) {
    s.rotaryAccum += ev.delta;
  }
}

static void screenTick(uint32_t) {
  tick();
}

static const ::scene::Screen kScreenTetris = {
    "Tetris",
    nullptr,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "game",
    buildTetrisScene,
    &screen_chrome_layout::kTetrisGameTopBarColor,
};

static const ::scene::Screen* const kScreens[] = {&kScreenTetris};
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Tetris", "tetris", &kScreenTetris, prepareWorkflow, prepareWorkflow, exitApp,
                            "src/apps/tetris_app.cpp", kScreens, nullptr, nullptr);

}  // namespace tetris
}  // namespace apps

#endif  // MENU_APP
