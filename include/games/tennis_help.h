#pragma once
/**
 * Tennis "how to play" panel: its pages, when it may open, and its paging. Opened by holding A (games/hold_help.h)
 * while the game waits for the player; drawn by games/tennis_view. Pure: host-tested in test/test_tennis_help.
 */
#include <stdint.h>

#include "assets/doodle_notebook.h"
#include "core/tennis.h"

namespace games {
namespace tennis_help {

// ---- layout (games/tennis_view draws it) ----
constexpr int kCardX = 10, kCardY = 44, kCardW = 260, kCardH = 190;
constexpr int kTextX = kCardX + 12, kTextW = kCardW - 24;
inline const doodle_art::Font& titleFont() { return doodle_art::font16; }
inline const doodle_art::Font& headingFont() { return doodle_art::font16; }
inline const doodle_art::Font& bodyFont() { return doodle_art::font13; }
inline const doodle_art::Font& footerFont() { return doodle_art::font13; }

constexpr const char* kTitle = "How to play: Tennis";
constexpr const char* kFooter = "dial: next page   A, B or press: close";
constexpr const char* kHint = "hold A: how to play";

// ---- pages (they describe core/tennis and apps/tennis_app as they are) ----
constexpr int kLines = 6;
struct Page {
  const char* heading;
  const char* lines[kLines];   // nullptr ends the page early
};
constexpr int kPageCount = 4;
constexpr Page kPages[kPageCount] = {
    {"Getting started",
     {"Hold the kami like a racket handle.", "Hold it still to calibrate;", "B calibrates again at any time.",
      "Press the dial to toss the ball up,", "swing as it peaks for a power serve.", "A+B leaves the game."}},
    {"Rallies",
     {"You run to the ball by yourself:", "just swing as it reaches you.", "Forehand: racket from right to left.",
      "Backhand: from left to right.", "Swing harder for a faster ball.", nullptr}},
    {"Aiming",
     {"Your timing aims the shot.", "Early: cross-court; late: down-line.",
      "Your natural timing goes straight.", "Make the CPU run side to side:", "a stretched CPU misses, or sends", "back an easy ball."}},
    {"Scoring",
     {"Points go 15, 30, 40, game.", "At 40-40 (deuce) win two in a row.", "First to 3 games wins the match.",
      "Serves swap every game. A toss", "missed or let drop is a fault;", "two faults lose the point."}},
};

// ---- when it may open ----
enum class Mode : uint8_t { NoImu, Calibrating, Playing };

// Only while the game waits for the player: calibrating, the player's serve, between points, the match over. Never
// during a toss, a rally, or the moment before the CPU serves.
inline bool canOpen(Mode mode, const core::tennis::State& st) {
  if (mode != Mode::Playing) return true;
  using core::tennis::Phase;
  return (st.phase == Phase::ServeReady && st.playerServes) || st.phase == Phase::PointOver ||
         st.phase == Phase::MatchOver;
}

// ---- paging ----
class Panel {
 public:
  void open(uint32_t ms) {
    open_ = true;
    page_ = 0;
    openedAt_ = ms;
  }
  // Closes it; returns how long it was open (the game was paused that long).
  uint32_t close(uint32_t ms) {
    if (!open_) return 0;
    open_ = false;
    return ms - openedAt_;
  }
  void turn(int delta) {
    if (!open_) return;
    int p = page_ + (delta > 0 ? 1 : (delta < 0 ? -1 : 0));
    page_ = (uint8_t)(p < 0 ? 0 : (p >= kPageCount ? kPageCount - 1 : p));
  }
  bool isOpen() const { return open_; }
  int page() const { return page_; }

 private:
  uint32_t openedAt_ = 0;
  uint8_t page_ = 0;
  bool open_ = false;
};

}  // namespace tennis_help
}  // namespace games
