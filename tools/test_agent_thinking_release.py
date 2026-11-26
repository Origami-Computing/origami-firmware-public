"""Thinking keeps a push-to-talk turn the server ended while A+B were still held.

The server's endpoint can finish recording before the person lets go of A+B.
Releasing those buttons in Thinking must not cancel the reply; a button pressed
after Thinking began still cancels exactly as before. Compiles the production
onEnter/onInput from screen_thinking.cpp against small stubs.
"""
from pathlib import Path
import subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    a = source.index(signature)
    b = source.index('{', a) + 1
    depth = 1
    while depth:
        depth += (source[b] == '{') - (source[b] == '}')
        b += 1
    return source[a:b] + '\n'


screen = (ROOT / 'src/apps/agent/screen_thinking.cpp').read_text()
state = screen[screen.index('static uint32_t sThinkingEnteredMs'):screen.index('void onEnter(')]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
uint32_t millis(){return 0;}
namespace hal { namespace input {
enum class EventType { ButtonA_Press, ButtonA_Release, ButtonA_Tap, ButtonB_Press, ButtonB_Release, ButtonB_Tap, ButtonC_Tap };
struct InputEvent { EventType type; };
bool a=false,b=false;
bool buttonADown(){return a;} bool buttonBDown(){return b;}
}}
enum class CancelTarget { Eyes, Menu };
struct AgentState { bool handsFree=false, handsFreeReady=false; uint8_t thinkDotPhase=0; char thinkStatus[64]={}; };
namespace motion { void cancel(){} }
namespace render { void paintThinkingShell(AgentState&,int){} }
namespace flow { int cancels=0; CancelTarget last=CancelTarget::Eyes;
  void cancelSend(CancelTarget t){++cancels;last=t;} }
''' + state + function(screen, 'void onEnter(AgentState& s)') + function(screen, 'void onInput(AgentState& s, hal::input::InputEvent ev)') + r'''
using ET = hal::input::EventType;
void send(AgentState& s, ET t){ onInput(s, hal::input::InputEvent{t}); }
int main() {
    AgentState s;
    // Endpoint ended the turn while A+B were held: their release is not a cancel.
    hal::input::a = hal::input::b = true; onEnter(s);
    hal::input::b = false; send(s, ET::ButtonB_Release);
    hal::input::a = false; send(s, ET::ButtonA_Release);
    send(s, ET::ButtonA_Tap);
    assert(flow::cancels == 0);
    // A fresh A press then release cancels to eyes, as before.
    send(s, ET::ButtonA_Press); send(s, ET::ButtonA_Release);
    assert(flow::cancels == 1 && flow::last == CancelTarget::Eyes);
    // Held turn, then a fresh B press cancels to the menu.
    flow::cancels = 0; hal::input::a = true; onEnter(s);
    send(s, ET::ButtonB_Press);
    assert(flow::cancels == 1 && flow::last == CancelTarget::Menu);
    // Entered with nothing held: unchanged behaviour.
    flow::cancels = 0; hal::input::a = hal::input::b = false; onEnter(s);
    send(s, ET::ButtonA_Release);
    assert(flow::cancels == 1 && flow::last == CancelTarget::Eyes);
    send(s, ET::ButtonC_Tap); assert(flow::cancels == 1);
    std::puts("PASS Thinking ignores releasing A/B held over from an endpointed push-to-talk turn; fresh A/B presses still cancel");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.cpp'
    src.write_text(code)
    subprocess.check_call(['c++', '-std=c++17', '-fsanitize=address,undefined', '-o', Path(tmp) / 'test', src])
    print(subprocess.check_output([Path(tmp) / 'test'], text=True), end='')
