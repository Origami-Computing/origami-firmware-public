#pragma once
/**
 * Hold A to open a game's "how to play" panel, like the Claude app's hold-B actions panel. Every motion game shows
 * a "hold A: how to play" hint and opens its instructions after a 600 ms hold of A.
 *
 * Event order from hal_input: ButtonA_Press ... ButtonA_Release, then ButtonA_Tap (a tap comes after every release
 * that wasn't part of an A+B chord, however long the press). So after a long hold the game must ignore both the
 * release and the tap that follows it:
 *   ButtonA_Press   -> press(ms)
 *   each tick       -> if (tick(ms)) open the panel
 *   ButtonA_Release -> if (release(ms)) { if (opensOnRelease()) open the panel; ignore it }
 *   ButtonA_Tap     -> if (swallowTap()) ignore it; else the game's normal A tap
 *   Chord_AB        -> cancel()   (A+B exits the app)
 * Pure C++, host-tested in test/test_hold_help.
 */
#include <stdint.h>

namespace games {

class HoldToOpen {
 public:
  static constexpr uint32_t kHoldMs = 600;

  void press(uint32_t ms) {
    down_ = true;
    downAt_ = ms;
    fired_ = openOnRelease_ = false;
  }
  void cancel() { down_ = fired_ = false; }
  // True once, when the hold reaches kHoldMs.
  bool tick(uint32_t ms) {
    if (!down_ || fired_ || (int32_t)(ms - downAt_) < (int32_t)kHoldMs) return false;
    fired_ = true;
    return true;
  }
  // True if this press was a long hold: ignore the release (and swallowTap() the tap after it).
  bool release(uint32_t ms) {
    const bool longHold = down_ && (fired_ || (int32_t)(ms - downAt_) >= (int32_t)kHoldMs);
    openOnRelease_ = longHold && !fired_;                  // no tick saw it: open now
    down_ = fired_ = false;
    swallowTap_ = longHold;
    return longHold;
  }
  bool opensOnRelease() {
    const bool o = openOnRelease_;
    openOnRelease_ = false;
    return o;
  }
  bool swallowTap() {
    const bool s = swallowTap_;
    swallowTap_ = false;
    return s;
  }
  bool held() const { return down_; }

 private:
  uint32_t downAt_ = 0;
  bool down_ = false, fired_ = false, swallowTap_ = false, openOnRelease_ = false;
};

}  // namespace games
