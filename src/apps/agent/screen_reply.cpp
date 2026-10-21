#include "apps/agent/agent_motion.h"
#include "config/screen_chrome_layout.h"
#include "apps/agent/agent_screens.h"

#include <Arduino.h>
#include <esp_random.h>

#include "apps/agent/agent_config.h"
#include "apps/agent/agent_flow.h"
#include "apps/agent/agent_emotion.h"
#include "apps/agent/agent_render.h"
#include "core/str_utils.h"
#include "hal/hal_input.h"
#include "ui/text_wrap.h"

namespace apps {
namespace agent {

namespace {

// Reply screen: dialogue box typewriter with mini-eye avatar above.
// Per-character animation is driven by render::serviceTypewriter; the screen
// only handles input (segment paging, line scroll, return-to-eyes).
//
// "Seen once" rule: a segment animates with typewriter + SFX exactly the first
// time the user reaches it in a given reply session. Revisiting via A (back)
// or via B (forward into an already-seen index) snaps to the fully revealed
// view. s_maxSeenSegIdx is the high-water mark — reset to 0 every time the
// screen is (re-)entered, since each entry is a fresh reply.
static int s_maxSeenSegIdx = 0;

void scheduleNextMiniBlink(AgentState& s, uint32_t nowMs) {
    s.miniBlinkMs = nowMs + cfg::agent::kMiniBlinkMinOpenMs +
                    (esp_random() % (cfg::agent::kMiniBlinkMaxOpenMs -
                                     cfg::agent::kMiniBlinkMinOpenMs));
}

void onEnter(AgentState& s) {
    render::computeDialogueLayout(s);
    if(s.restoringReply){
        s.restoringReply=false;s_maxSeenSegIdx=s.conv.segmentCount()-1;
        s.miniEyesOpen=true;scheduleNextMiniBlink(s,millis());
        motion::cancel();render::paintReplyShell(s);
        render::snapTypewriterToView(s);render::paintReplyText(s);return;
    }
    s.msgIndex      = 0;
    s.scrollLines   = 0;
    s_maxSeenSegIdx = 0;  // segment 0 is about to be revealed for the first time
    render::resetTypewriter(s);

    s.miniEyesOpen = true;
    scheduleNextMiniBlink(s, millis());

    motion::startReply();
    motion::tick(s,millis());
}

void onExit(AgentState& /*s*/) {
}

void onTick(AgentState& s, uint32_t nowMs) {
    flow::pollCompletion();
    if(flow::showInboxReply())return;

    if(s.stage!=Stage::Reply || motion::tick(s,nowMs))return;
    render::serviceTypewriter(s, nowMs);
    flow::pollReturnHomeAfterReply(nowMs, render::typewriterRevealed(s));
    if (s.stage != Stage::Reply) return;
    const auto expression=emotion::snapshot(s.msgIndex);
    if(expression.revision!=s.miniEmotionRevision||s.msgIndex!=s.miniEmotionSegment)
        render::paintMiniEyes(s);

    // Sample the short squeezed frame throughout the blink, not just at its ends.
    if ((nowMs - s.lastAnim) >= 32u) {
        s.animFrame++;
        s.lastAnim = nowMs;
        if (nowMs >= s.miniBlinkMs) {
            if (s.miniEyesOpen) {
                s.miniEyesOpen = false;
                s.miniBlinkStarted = nowMs;
                s.miniBlinkMs  = nowMs + 250u;
            } else {
                s.miniEyesOpen = true;
                scheduleNextMiniBlink(s, nowMs);
            }
        }
        render::paintMiniEyes(s);
    }
}

// ── Scroll helpers ──────────────────────────────────────────────────────────

bool scrollByLines(AgentState& s, int delta) {
    if (delta == 0)                       return false;
    if (s.agentTextW <= 0)                 return false;
    if (s.conv.segmentCount() <= 0)        return false;
    if (!s.conv.ensure())                  return false;

    // Compute the highest valid scrollLines value for the current segment.
    // (Was a `bounds` lambda when scroll could span segments; we only need the
    // current segment's maxStart now.)
    int maxStart = 0;
    if (s.msgIndex >= 0 && s.msgIndex < s.conv.segmentCount()) {
        const char* seg = s.conv.segment(s.msgIndex);
        if (seg && seg[0]) {
            core::str::utf8LossyToAscii(seg, s.conv.wrapScratch(),
                                        s.conv.wrapScratchSize());
            const int n = ui::text::countWrapped(s.conv.wrapScratch(),
                                                 s.agentTextW, 2);
            maxStart = (n > s.agentMaxLines) ? (n - s.agentMaxLines) : 0;
        }
    }

    // Scroll is single-segment now: clamp to [0, maxStart]. Crossing into the
    // next/previous segment is done explicitly via B (forward, animated) or A
    // (back, snapped) — see onInput below.
    int ns = s.scrollLines + delta;
    if (ns < 0)        ns = 0;
    if (ns > maxStart) ns = maxStart;
    if (ns == s.scrollLines) return false;
    s.scrollLines = ns;
    render::snapTypewriterToView(s);
    render::paintReplyText(s);
    return true;
}

bool advancePage(AgentState& s) {
    if (s.conv.segmentCount() > 1 && s.msgIndex + 1 < s.conv.segmentCount()) {
        const int next = s.msgIndex + 1;
        s.msgIndex = next;
        s.scrollLines = 0;
        if (next > s_maxSeenSegIdx) {
            s_maxSeenSegIdx = next;
            render::resetTypewriter(s);
        } else {
            render::snapTypewriterToView(s);
        }
        render::paintReplyText(s);
        return true;
    }

    return false;
}

void onInput(AgentState& s, hal::input::InputEvent ev) {
    flow::keepReply();
    using ET = hal::input::EventType;
    if(ev.type==ET::Scroll || ev.type==ET::ButtonA_Tap || ev.type==ET::ButtonB_Tap)
        motion::completeReply(s);

    if (ev.type == ET::Scroll && ev.delta != 0) {
        scrollByLines(s, ev.delta);
        return;
    }
    if (ev.type == ET::ButtonA_Tap) {
        // A goes BACK through segments. On the first segment there is nothing
        // to go back to, so fall through to the legacy "A exits" gesture.
        if (s.msgIndex > 0) {
            s.msgIndex--;
            s.scrollLines = 0;
            // snapTypewriterToView fills twRevealLen to the full visible
            // string length, so serviceTypewriter short-circuits on the very
            // next tick — no per-char reveal, no SFX. Re-visiting a previous
            // segment should feel instant, not replay the animation.
            render::snapTypewriterToView(s);
            render::paintReplyText(s);
        } else {
            flow::returnToHome();
        }
        return;
    }
    if (ev.type == ET::ButtonB_Tap) {
        if (advancePage(s)) return;
        flow::returnToHome();
        return;
    }
}

bool onChord(AgentState& s) {
    // Follow-up capture is one transition; do not play the home/idle scene first.
    s.recStartedByChord = true;
    flow::startRecording();
    return true;
}

void enterScreen() {
    onEnter(flow::state());
}

void exitScreen() {
    onExit(flow::state());
}

void tickScreen(uint32_t nowMs) {
    onTick(flow::state(), nowMs);
    serviceScreenTick(nowMs);
}

void inputScreen(hal::input::InputEvent ev) {
    prepareScreenInput(ev);
    onInput(flow::state(), ev);
}

bool chordScreen() {
    return onChord(flow::state());
}

}  // namespace

// External accessors used by agent_app for legacy public API.

bool replyScrollByLines(int delta) {
    return scrollByLines(flow::state(), delta);
}

bool replyAdvancePage() {
    return advancePage(flow::state());
}

const ::scene::Screen kScreenReply = {
    /*name   */ "Reply",
    /*onEnter*/ enterScreen,
    /*onExit */ exitScreen,
    /*onTick */ tickScreen,
    /*onInput*/ inputScreen,
    /*onChord*/ chordScreen,
    /*onDraw */ nullptr,
    /*id     */ "reply",
    /*builder*/ render::buildReplyScene,
    &screen_chrome_layout::kKamiReplyTopBarColor,
};

}  // namespace agent
}  // namespace apps
