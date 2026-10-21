#pragma once
/**
 * Golf's "how to play" panel: hold A (games::HoldToOpen) while the game waits for you; the dial turns the pages and
 * a tap of A, B or C closes it. HelpController applies golf's input rules (pure, host-tested in test/test_golf):
 *   - a long A opens the panel (only while waiting) and its release and tap are swallowed: no club change;
 *   - a short A is still the game's A tap (previous club);
 *   - while open, every input is swallowed (a B press must not start a swing); A, B or C tap closes.
 */
#include <stdint.h>

#include "games/hold_help.h"

namespace ui {
namespace golf {

enum class HelpEvent : uint8_t { APress, ARelease, ATap, BPress, BRelease, BTap, CPress, CRelease, CTap, Scroll, ChordAB, Other };
enum class HelpAction : uint8_t { Pass, Swallow, Opened, Closed, Paged };

struct HelpPage {
  const char* title;
  const char* lines[7];
};
int helpPageCount();
const HelpPage& helpPage(int i);

class HelpController {
 public:
  bool isOpen() const { return open_; }
  int page() const { return page_; }
  // `waiting`: golf is waiting for the player (not mid-swing or mid-flight). Pass = the game handles the event.
  HelpAction event(HelpEvent e, uint32_t ms, bool waiting, int delta = 0);
  bool tick(uint32_t ms, bool waiting);   // true when a hold opens the panel
  void open();
  void close() { open_ = false; }

 private:
  games::HoldToOpen hold_;
  bool open_ = false;
  int page_ = 0;
};

}  // namespace golf
}  // namespace ui
