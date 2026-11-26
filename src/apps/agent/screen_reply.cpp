#include "services/agent_speech.h"
#include "sfx/product_sounds.h"
#include "services/agent_transport.h"
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
#include "ui/agent_conversation.h"

namespace apps {
namespace agent {

namespace {

// Streamed text is readable immediately. The wheel scrolls measured lines;
// A/B preserve existing segment navigation and the chord starts a follow-up.
void scheduleNextMiniBlink(AgentState& s, uint32_t nowMs) {
    s.miniBlinkMs = nowMs + cfg::agent::kMiniBlinkMinOpenMs +
                    (esp_random() % (cfg::agent::kMiniBlinkMaxOpenMs -
                                     cfg::agent::kMiniBlinkMinOpenMs));
}

void onEnter(AgentState& s) {
    render::computeDialogueLayout(s);
    if(s.restoringReply){
        s.restoringReply=false;s.followReply=false;s.viewInit=false;s.replyDoneCue=true;
        s.miniEyesOpen=true;scheduleNextMiniBlink(s,millis());
        motion::cancel();render::paintReplyShell(s);
        render::snapTypewriterToView(s);render::paintReplyText(s);return;
    }
    s.msgIndex      = 0;
    s.scrollLines   = 0;
    // Keep the opening words readable as live chunks arrive. Explicitly
    // scrolling to the bottom opts into following subsequent text.
    s.followReply = false;
    s.replyDoneCue = false;
    s.replyPainted = s.replyRevision;
    render::resetTypewriter(s);

    s.miniEyesOpen = true;
    scheduleNextMiniBlink(s, millis());

    motion::cancel();
    render::paintReplyShell(s);
}

void onExit(AgentState& /*s*/) {
}

void onTick(AgentState& s, uint32_t nowMs) {
    flow::pollCompletion();
    services::agent_speech::tick(nowMs);
    if(flow::serviceTurnGesture(nowMs))return;
    if(flow::continueHandsFree())return;
    if(flow::showInboxReply())return;

    if(s.stage!=Stage::Reply || motion::tick(s,nowMs))return;
    render::serviceTypewriter(s, nowMs);
    flow::pollReturnHomeAfterReply(nowMs, render::typewriterRevealed(s));
    if (s.stage != Stage::Reply) return;
    const auto expression=emotion::snapshot(s.msgIndex);
    if(expression.revision!=s.miniEmotionRevision||s.msgIndex!=s.miniEmotionSegment)
        render::paintMiniEyes(s);
    // Push-to-talk turn finished and fully shown: "your turn" water drop.
    if(!s.replyDoneCue && !s.continuous && !flow::isSessionActive() && render::typewriterRevealed(s)) {
        s.replyDoneCue=true;
        if(!services::agent_speech::busy()) flow::cue((uint8_t)sfx::product::Sound::WaterDrop);
    }
}

// ── Scroll helpers ──────────────────────────────────────────────────────────

bool scrollByLines(AgentState& s, int delta) {
    if(s.documentVisible && s.documentText) {
        const int maximum=std::max(0,ui::agent_conversation::count(s.documentText)-4);
        s.documentScroll=std::max(0,std::min(maximum,s.documentScroll+delta));
        render::paintReplyText(s);return true;
    }
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
            const int n = ui::agent_conversation::count(s.conv.wrapScratch());
            maxStart = (n > s.agentMaxLines) ? (n - s.agentMaxLines) : 0;
        }
    }

    // Scroll is single-segment now: clamp to [0, maxStart]. Crossing into the
    // next/previous segment is done explicitly via B (forward) or A
    // (back) — see onInput below.
    int ns = s.scrollLines + delta;
    if (ns < 0)        ns = 0;
    if (ns > maxStart) ns = maxStart;
    s.followReply=ns==maxStart;
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
        s.followReply=false;
        render::snapTypewriterToView(s);
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

    if (ev.type == ET::ButtonC_Tap) return;   // C keeps its OS meaning only
    if (ev.type == ET::Scroll && ev.delta != 0) {
        scrollByLines(s, ev.delta);
        return;
    }
    if(s.documentVisible && (ev.type==ET::ButtonA_Tap || ev.type==ET::ButtonB_Tap)) {
        s.documentVisible=false;render::paintReplyShell(s);return;
    }
    if (ev.type == ET::ButtonA_Tap) {
        // A goes BACK through segments. On the first segment there is nothing
        // to go back to, so fall through to the legacy "A exits" gesture.
        if (s.msgIndex > 0) {
            s.msgIndex--;
            s.scrollLines = 0;
            s.followReply=false;
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
    // Follow-up: hold for push-to-talk, tap for agent mode (decided in onTick).
    (void)s;
    return flow::beginTurnGesture();
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
