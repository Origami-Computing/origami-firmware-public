#pragma once
/**
 * Bowling's "how to play" panel: when it opens, and how it shares the buttons with the game. Hold A (600 ms,
 * games::HoldToOpen) while the game waits for the player (aiming, between balls, game over, calibrating) to open it;
 * a hold while the ball rolls is ignored. While it is open the dial turns pages, a tap of A, B or C closes it, and
 * every other button event is swallowed (no swing, no step). The release and tap after the opening hold are
 * swallowed too, so A's step-left does not fire. Pure C++, host-tested in test/test_bowling_help.
 */
#include <stdint.h>

#include "games/hold_help.h"

namespace games {
namespace bowling_help {

constexpr int kPages = 3;

enum class Ev : uint8_t { APress, ARelease, ATap, BPress, BRelease, BTap, CPress, CRelease, CTap, Scroll, Chord };
enum class Act : uint8_t {
  Pass,      // not the panel's: the game handles it as before
  Swallow,   // the panel's (or the opening hold's): ignore it
  Opened,
  Closed,
  Paged,
};

class Help {
 public:
  bool isOpen() const { return open_; }
  int page() const { return page_; }
  void open() { open_ = true; page_ = 0; }
  void close() { open_ = false; }
  void toggle() { if (open_) close(); else open(); }

  // Each tick: opens once a hold of A reaches 600 ms while the game waits.
  Act tick(uint32_t ms, bool waiting) {
    if (hold_.tick(ms) && waiting && !open_) { open(); return Act::Opened; }
    return Act::Pass;
  }

  Act input(Ev e, uint32_t ms, bool waiting, int delta = 0) {
    switch (e) {
      case Ev::Chord:
        hold_.cancel();
        return Act::Pass;
      case Ev::APress:
        if (open_) return Act::Swallow;                    // the start of a closing tap
        hold_.press(ms);
        return Act::Pass;
      case Ev::ARelease: {
        if (hold_.release(ms)) {
          if (hold_.opensOnRelease() && waiting && !open_) { open(); return Act::Opened; }
          return Act::Swallow;
        }
        return open_ ? Act::Swallow : Act::Pass;
      }
      case Ev::ATap:
        if (hold_.swallowTap()) return Act::Swallow;
        if (open_) { close(); return Act::Closed; }
        return Act::Pass;
      case Ev::BTap:
      case Ev::CTap:
        if (open_) { close(); return Act::Closed; }
        return Act::Pass;
      case Ev::Scroll:
        if (!open_) return Act::Pass;
        page_ += delta;
        page_ = page_ < 0 ? 0 : (page_ >= kPages ? kPages - 1 : page_);
        return Act::Paged;
      default:                                             // B/C press and release
        return open_ ? Act::Swallow : Act::Pass;
    }
  }

 private:
  HoldToOpen hold_;
  bool open_ = false;
  int page_ = 0;
};

}  // namespace bowling_help
}  // namespace games
