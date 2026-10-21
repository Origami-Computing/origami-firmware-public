// Golf's "how to play" panel: see include/ui/golf_help.h.
#include "ui/golf_help.h"

namespace ui {
namespace golf {

namespace {
// Keep in step with the game (spec: docs/superpowers/specs/2026-09-30-golf-design.md).
const HelpPage kPages[] = {
    {"Getting ready",
     {"Dial: aim left or right.", "Dial press: next club; the ring shows",
      "where that club lands.", "Tap A: practice mode (nothing is hit).",
      "Tap B: see the whole hole.", "Hold A+B: leave the game.", nullptr}},
    {"The swing",
     {"Hold B: however you hold the kami now", "is your neutral (it re-centres each time).",
      "Then swing it forward, like a club:", "harder = further. A gentle wave or slow",
      "aiming is not a stroke. Let go of B", "to cancel.", nullptr}},
    {"The Shot Meter",
     {"On the left: it fills with how hard you", "swing (the dots are 25 / 50 / 75 / 100 %).",
      "It bends like your swing: square is", "straight, a tilt bends it the way the ball",
      "will curve. Much too hard: it overshoots", "red, whipping to the side the ball drifts.", nullptr}},
    {"The line",
     {"While you hold B the line shows where the", "ball will go, and moves as you swing:",
      "swing to the left or right: it swings", "(pull or push); tilt the kami: it bends",
      "(hook or slice). At impact it freezes", "on your shot.", nullptr}},
    {"Putting",
     {"On the green you putt: the same swing,", "smaller. 100% rolls the yards shown", "on the club box (yd max).",
      "Moving dots show the slope:", "the ball breaks downhill.", "Within 2 feet of the hole is a gimme.", nullptr}},
    {"The course",
     {"Wind (top right) carries the ball.", "Rough and waste ground cost distance;",
      "in a bunker, use the sand wedge.", "Water or canyon: +1 stroke, drop.", "Out of bounds: +1, hit again.",
      "Trees knock a low ball down.", nullptr}},
    {"Practice",
     {"Tap A while aiming: practice mode.", "Swing with B as usual: nothing is hit.",
      "Title > Swing Practice: swing as often as", "you like; each shows its line and yards.",
      "Title > Driving Range: targets 50-250 yd.", "Three courses of 9 holes: par 36.", nullptr}},
};
constexpr int kPageCount = (int)(sizeof(kPages) / sizeof(kPages[0]));
}  // namespace

int helpPageCount() { return kPageCount; }
const HelpPage& helpPage(int i) { return kPages[i < 0 ? 0 : (i >= kPageCount ? kPageCount - 1 : i)]; }

void HelpController::open() {
  open_ = true;
  page_ = 0;
}

bool HelpController::tick(uint32_t ms, bool waiting) {
  if (!hold_.tick(ms) || !waiting || open_) return false;
  open();
  return true;
}

HelpAction HelpController::event(HelpEvent e, uint32_t ms, bool waiting, int delta) {
  if (e == HelpEvent::ChordAB) {
    hold_.cancel();
    return HelpAction::Pass;
  }
  if (open_) {
    switch (e) {
      case HelpEvent::ARelease:
        hold_.release(ms);                  // the hold that opened it (its tap is swallowed below)
        return HelpAction::Swallow;
      case HelpEvent::ATap:
        if (hold_.swallowTap()) return HelpAction::Swallow;
        open_ = false;
        return HelpAction::Closed;
      case HelpEvent::BTap:
      case HelpEvent::CTap:
        open_ = false;
        return HelpAction::Closed;
      case HelpEvent::Scroll: {
        const int p = page_ + (delta > 0 ? 1 : (delta < 0 ? -1 : 0));
        page_ = p < 0 ? 0 : (p >= kPageCount ? kPageCount - 1 : p);
        return HelpAction::Paged;
      }
      default:
        return HelpAction::Swallow;         // presses, releases, anything else: not for the game
    }
  }
  switch (e) {
    case HelpEvent::APress:
      hold_.press(ms);
      return HelpAction::Pass;
    case HelpEvent::ARelease:
      if (!hold_.release(ms)) return HelpAction::Pass;
      if (hold_.opensOnRelease() && waiting) {
        open();
        return HelpAction::Opened;
      }
      return HelpAction::Swallow;
    case HelpEvent::ATap:
      return hold_.swallowTap() ? HelpAction::Swallow : HelpAction::Pass;
    default:
      return HelpAction::Pass;
  }
}

}  // namespace golf
}  // namespace ui
