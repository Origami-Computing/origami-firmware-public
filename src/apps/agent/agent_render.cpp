#include "services/agent_transport.h"
#include "ui/agent_conversation.h"
#include "services/agent_speech.h"
#include "services/agent_latency.h"
#include "ui/default_font_metrics.h"
#include "apps/agent/agent_motion.h"
#include "services/agent_voice.h"
#include "apps/agent/agent_render.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdio.h>

#include "apps/agent/agent_config.h"
#include "apps/agent/agent_flow.h"
#include "apps/agent/agent_emotion.h"
#include "apps/agent_app.h"
#include "config/app_config.h"
#include "config/agent_layout.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "core/memory.h"
#include "core/str_utils.h"
#include "hal/hal_display.h"
#include "sfx/sfx.h"
#include "sfx/sfx_typewriter.h"
#include "sfx/product_sounds.h"
#include "ui/agent_overlays.h"
#include "ui/dialogue_box.h"
#include "ui/home_eyes.h"
#include "ui/timer_controls.h"
#include "services/countdown.h"
#include "ui/agent_transition_motion.h"
#include "ui/kami_motion.h"
#include "ui/scene.h"
#include "ui/text_wrap.h"
#include "ui/kami_dialogue.h"
#include "assets/kami_dialogue.h"
#include "assets/boot_double_blink_kami.h"
#include "ui/alpha_bitmap.h"
#include "ui/color.h"
#include "ui/listening_style.h"

namespace apps {
namespace agent {
namespace render {

static inline TFT_eSPI& T() { return hal::display::tft(); }
static inline int       scrW() { return T().width(); }
static inline int       scrH() { return T().height(); }

void buildEyesScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("eyes.sprite",
                  {HOME_SCREEN_PUSH_X, HOME_SCREEN_PUSH_Y, HOME_SCREEN_W, HOME_SCREEN_H},
                  TFT_WHITE, 0, 0);
    scene.addRect("eyes.frame",
                  {HOME_SCREEN_PUSH_X, HOME_SCREEN_PUSH_Y, HOME_SCREEN_W, HOME_SCREEN_FRAME_H},
                  kUiFg, 12, 1);
    scene.addRect("eyes.band",
                  {HOME_SCREEN_PUSH_X, (int16_t)(HOME_SCREEN_PUSH_Y + HOME_SCREEN_BAND_OFFSET),
                   HOME_SCREEN_W, HOME_SCREEN_BAND_H},
                  kUiBg, 0, 2);
    scene.addLabel("layout.constants", {8, (int16_t)(height - 18), (int16_t)(width - 16), 12},
                   "Eye sprite layout", kUiMuted, kUiBg, 1,
                   ui::scene::TextAlign::TopLeft, 3);
}

// Tooling previews mirror the full-panel conversation layout (no status bar).
void buildRecordingScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("conversation.bg",{0,0,width,height},kUiBg,0,0);
    scene.addRect("conversation.eyes",{40,40,200,116},kUiBg,0,1);
    scene.addLabel("conversation.line",{24,196,232,20},"Listening...",kUiMuted,kUiBg,2,ui::scene::TextAlign::TopCenter,2);
}

void buildThinkingScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("conversation.bg",{0,0,width,height},kUiBg,0,0);
    scene.addRect("conversation.eyes",{40,40,200,116},kUiBg,0,1);
    scene.addLabel("conversation.line",{24,196,232,20},"Your words stay here, dimmed",kUiMuted,kUiBg,2,ui::scene::TextAlign::TopCenter,2);
}

void buildReplyScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("conversation.bg",{0,0,width,height},kUiBg,0,0);
    scene.addRect("conversation.eyes",{100,16,80,48},kUiBg,0,1);
    scene.addLabel("conversation.reply",{24,84,232,120},"Replies appear as they arrive",kUiFg,kUiBg,2,ui::scene::TextAlign::TopLeft,2);
}

void buildErrorScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    const auto kind=flow::state().noticeKind;
    scene.addRect("error.bg", {0,kSystemStatusBarH,width,(int16_t)(height-kSystemStatusBarH)},kUiBg,0,0);
    scene.addLabel("error.text",{36,90,208,110},notice::message(kind),kUiFg,kUiBg,2,ui::scene::TextAlign::TopLeft,1);
    scene.addLabel("error.footer",{36,224,208,12},notice::needsWifi(kind)?"A Back   B Wi-Fi":kind==notice::Kind::Auth?"A Back   B Settings":"A Back   A+B Retry",kUiFg,kUiBg,1,ui::scene::TextAlign::TopLeft,1);
}

static void publishEyesRuntimeScene(AgentState& s) {
    ui::scene::Scene& scene = ui::scene::scratchScene();
    scene.clear();
    ui::scene::setCurrentScene(&scene);

    int x = 0, y = kSystemStatusBarH, w = scrW(), h = scrH() - kSystemStatusBarH;
    (void)ui::home_eyes::currentFrameBounds(T(), &x, &y, &w, &h);

    ui::scene::Node* bg = scene.addRect(
        "eyes.bg",
        {0, kSystemStatusBarH, (int16_t)scrW(), (int16_t)(scrH() - kSystemStatusBarH)},
        TFT_WHITE, 0, 0);
    if (bg) bg->visible = false;

    ui::scene::Node* frame = scene.addRect(
        "eyes.frame",
        {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h},
        TFT_WHITE, 0, 1);
    if (frame) frame->visible = false;

    char offset[40];
    snprintf(offset, sizeof(offset), "offset %d,%d", (int)s.eyeFloatX, (int)s.eyeFloatY);
    ui::scene::Node* info = scene.addLabel(
        "eyes.offset",
        {8, (int16_t)(scrH() - 18), (int16_t)(scrW() - 16), 12},
        offset, kUiMuted, TFT_WHITE, 1, ui::scene::TextAlign::TopLeft, 2);
    if (info) info->visible = false;
}

namespace view = ui::agent_conversation;

void computeDialogueLayout(AgentState& s) {
    s.agentTextX = view::x; s.agentTextY = view::replyY; s.agentTextW = view::width;
    s.agentMaxLines = view::replyRows; s.agentLineH = view::pitch;
    s.agentTextH = view::replyRows * view::pitch;
}

// ─── Sprite lifecycle ────────────────────────────────────────────────────────

void releaseChatSprites(AgentState& s) {
    s.spritesReady = false;
    core::memory::freeSprite(s.sprMiniEye);
    core::memory::freeSprite(s.sprDialogue);
    s.sprDialogueW = s.sprDialogueH = 0;
}

// ─── Conversation view ───────────────────────────────────────────────────────
//
// One painter for listening, thinking and replying. compose() decides what the
// panel shows from workflow state; paintConversation() draws the whole panel
// (the status bar is hidden outside Home) and skips frames where nothing
// visible changed. Eye size eases between the listening and reply sizes.

namespace {
enum class Mode : uint8_t { Listening, Thinking, Reply, Answer }; // Answer: one line, big
struct Content {
    Mode mode = Mode::Listening;
    const char* line = nullptr; bool interim = false, dim = false;
    const char* hint = nullptr;
    const char* reply = nullptr; int scroll = 0, rows = view::replyRows;
    const char* answer = nullptr; int answerPx = 0;
    bool sub = false;          // agent mode: line/hint drawn small under the answer
    bool dot = false;
    int page = 0, pages = 0;   // one message per screen
};

float easeOut(float p) { p = p < 0 ? 0 : p > 1 ? 1 : p; const float q = 1 - p; return 1 - q * q * q; }

// Reply prefix revealed so far; copied into the conversation's wrap scratch so
// the display buffer stays intact. Falls back to the full text if it can't fit.
const char* revealedReply(AgentState& s) {
    s.conv.refreshDisplay(s.msgIndex);
    const char* full = s.conv.displayText();
    const size_t n = strlen(full);
    char* scratch = s.conv.wrapScratch();
    if (s.twRevealLen >= n || !scratch || s.conv.wrapScratchSize() <= s.twRevealLen) return full;
    memcpy(scratch, full, s.twRevealLen); scratch[s.twRevealLen] = 0;
    return scratch;
}

static Content composeView(AgentState& s);
Content compose(AgentState& s) {
    Content c = composeView(s);
    if (s.eyesUp && c.mode == Mode::Answer) {
        // Eyes stay up: a one-line answer becomes ordinary reply text below them.
        c.mode = Mode::Reply; c.reply = c.answer; c.answer = nullptr; c.answerPx = 0;
        c.rows = c.sub ? view::replyRows - 1 : view::replyRows; c.sub = false; c.scroll = 0;
    }
    return c;
}
static Content composeView(AgentState& s) {
    using View = AgentState::ConversationView;
    Content c;
    c.dot = s.continuous && s.stage == Stage::Recording;
    if (s.stage == Stage::Reply) {
        // Decide on everything received so far, so a short answer keeps the big
        // eyes and a long one moves to the paragraph view as soon as it outgrows.
        s.conv.refreshDisplay(s.msgIndex);
        c.page = s.msgIndex; c.pages = s.conv.segmentCount();
        const int px = view::answerPx(s.conv.displayText());
        if (px) {
            c.mode = Mode::Answer; c.answer = revealedReply(s); c.answerPx = px;
            return c;
        }
        c.mode = Mode::Reply;
        c.reply = revealedReply(s);
        const int lines = view::count(c.reply), maxScroll = std::max(0, lines - c.rows);
        if (s.followReply && s.msgIndex == s.conv.segmentCount() - 1) s.scrollLines = maxScroll;
        else if (s.scrollLines > maxScroll) s.scrollLines = maxScroll;
        c.scroll = s.scrollLines;
        return c;
    }
    if (s.stage == Stage::Thinking) {
        c.mode = Mode::Thinking;
        if (s.transcript[0]) { c.line = s.transcript; c.dim = true; }
        return c;
    }
    if (s.continuous) {
        // Agent mode: Kami's last answer stays until a new turn really starts
        // (the server clears it on input_begin). Stray noise transcripts and
        // rejected turns show only in the bottom line, so the answer never
        // flickers away. Your live words sit under it.
        const bool wordsHeard = s.transcript[0] != 0;
        if (s.conv.anyNonEmpty()) {
            if (s.conversationView == View::Thinking && wordsHeard) {
                c.mode = Mode::Thinking; c.line = s.transcript; c.dim = true;
                return c;
            }
            // The newest message, with its position when Kami sent several.
            const int latest = std::max(0, s.conv.segmentCount() - 1);
            s.conv.refreshDisplay(latest);
            c.page = latest; c.pages = s.conv.segmentCount();
            const char* text = s.conv.displayText();
            if (wordsHeard && s.conversationView == View::User) { c.line = s.transcript; c.interim = !s.transcriptFinal; }
            else c.hint = "Listening...";
            if (const int px = view::answerPx(text)) {
                c.mode = Mode::Answer; c.answer = text; c.answerPx = px;
                c.sub = true;   // your live line moves under the big answer
                return c;
            }
            c.mode = Mode::Reply; c.reply = text; c.rows = view::replyRows - 1;
            const int maxScroll = std::max(0, view::count(text) - c.rows);
            if (s.listeningReplyFollowTail) s.previousScrollLines = maxScroll;
            s.previousScrollLines = std::min(s.previousScrollLines, maxScroll);
            c.scroll = s.previousScrollLines;
            return c;
        }
        if (s.conversationView == View::Thinking) {
            c.mode = Mode::Thinking;
            if (wordsHeard) { c.line = s.transcript; c.dim = true; }
            return c;
        }
    }
    if (s.transcript[0]) { c.line = s.transcript; c.interim = !s.transcriptFinal; }
    else c.hint = "Listening...";
    return c;
}

float eyeT(const AgentState& s, uint32_t now) {
    const float target = s.viewMode == (uint8_t)Mode::Reply || s.eyesUp ? 1.f : 0.f;
    const float duration = target > 0 ? 380.f : 300.f;
    const float p = easeOut((float)(now - s.viewSinceMs) / duration);
    return s.viewFromT + (target - s.viewFromT) * p;
}

// Every eye view uses the Home renderer's natural blink.
float blinkOpen(uint32_t now) { return ui::home_eyes::isLoaded() ? ui::home_eyes::sharedBlink(now) : 1.f; }
bool blinkClosed(uint32_t now) { return blinkOpen(now) < .5f; }

// Eye travel between the Home size and the reply size reuses the cached Home
// "travel" frames (as staging's agent motion did). kTravelEnd is how far along
// that path the reply size sits; the small resting eyes match its last frame.
constexpr float kTravelEnd = .75f;
constexpr float kSmallNominal = 88.f + (16.f / 2.2f - 88.f) * kTravelEnd;
constexpr float kSmallSpacing = .9f + .1f * kTravelEnd;
constexpr float kSmallCenterY = 46.f;
float travelOffsetY(float a) { return kSmallCenterY - (127.f - 75.f * kTravelEnd) * (a / kTravelEnd); }

void paintDocument(AgentState& s, uint32_t now, bool force) {
    using View = AgentState::ConversationView;
    const bool showingReply = s.conversationView == View::Reply && s.conv.anyNonEmpty();
    const uint32_t sig = 0x9e3779b9u ^ (uint32_t)s.documentScroll * 131u ^ s.replyRevision * 40503u ^
                         s.transcriptRevision * 2654435761u ^ (uint32_t)s.conversationView * 7u ^ (s.documentPreview ? 1u : 0u);
    if (!force && sig == s.viewSignature) return;
    if (!force && now - s.viewPaintMs < 30u) return;
    const auto pose = emotion::snapshot(s.msgIndex).pose;
    view::documentEyes(T(), pose, blinkClosed(now) ? .12f : 1.f);
    if (s.continuous) {
        s.conv.refreshDisplay(std::max(0, s.conv.segmentCount() - 1));
        const char* live = showingReply ? s.conv.displayText()
                         : s.conversationView == View::User && s.transcript[0] ? s.transcript : "Listening...";
        if (!live) live = "";
        if (strlen(live) > 140) live += strlen(live) - 140;
        char footer[160];
        snprintf(footer, sizeof(footer), "%s: %.145s", showingReply ? "Kami" : "You", live);
        view::document(T(), s.documentTitle, s.documentText, s.documentScroll, s.documentTruncated, footer, s.documentPreview);
    } else {
        view::document(T(), s.documentTitle, s.documentText, s.documentScroll, s.documentTruncated);
    }
    hal::display::markDirty(0, 240);
    s.viewSignature = sig; s.viewPaintMs = now; s.viewInit = false;
    s.transcriptPainted = s.transcriptRevision; s.replyPainted = s.replyRevision;
}
}  // namespace

// Text layer: everything except the eyes. Drawn after the eyes.
static void paintText(AgentState& s, const Content& c) {
    if (c.reply) {
        view::reply(T(), c.reply, c.scroll, c.rows);
        if (c.reply[0]) services::agent_latency::painted(view::replyY, view::replyY + c.rows * view::pitch);
    }
    if (c.answer) {
        view::answer(T(), c.answer, c.answerPx);
        if (c.answer[0]) services::agent_latency::painted(view::lineBaseline - 24, view::lineBaseline + 8);
    }
    if (c.sub) view::subLine(T(), c.line ? c.line : c.hint);
    else if (c.line) view::line(T(), c.line, c.interim, c.dim);
    else if (c.hint) view::hint(T(), c.hint);
    if (c.dot) view::liveDot(T(), c.sub ? view::subBaseline : view::lineBaseline);
    view::pageMarker(T(), c.page, c.pages);
}

void paintConversation(AgentState& s, uint32_t now, bool force) {
    if (s.documentVisible && s.documentText) { paintDocument(s, now, force); return; }
    const Content c = compose(s);
    const bool raising = c.mode == Mode::Reply && c.reply;
    if (!s.viewInit) {
        s.viewInit = true; s.viewMode = (uint8_t)c.mode; s.viewSinceMs = now;
        s.viewFromT = c.mode == Mode::Reply || s.eyesUp ? 1.f : 0.f; s.levelSmooth = 0; s.viewLayer = 0;
    } else if ((uint8_t)c.mode != s.viewMode) {
        s.viewFromT = eyeT(s, now); s.viewMode = (uint8_t)c.mode; s.viewSinceMs = now;
    }
    // Set after the starting point is captured so the rise still animates.
    if (raising) s.eyesUp = true;
    const float t = eyeT(s, now);
    const uint32_t age = now - s.viewSinceMs;
    const uint32_t contentSig = (uint32_t)c.mode * 0x45d9f3bu ^ s.transcriptRevision * 2654435761u ^
                                s.replyRevision * 40503u ^ (uint32_t)s.twRevealLen * 97u ^ (uint32_t)c.scroll * 131u ^
                                (s.transcriptFinal ? 0x1000u : 0u) ^ (c.dot ? 0x2000u : 0u) ^ (uint32_t)s.msgIndex * 7919u ^
                                (uint32_t)s.conversationView * 0x9e37u ^ (uint32_t)c.pages * 0x51u;
    const auto pose = c.mode == Mode::Reply || c.mode == Mode::Answer ? emotion::snapshot(s.msgIndex).pose : ui::eye_rig::kPresets[0].pose;

    // The microphone level lifts the eyes slightly: "I hear you" before words arrive.
    float bob = 0;
    if (c.mode == Mode::Listening && s.stage == Stage::Recording) {
        const float level = std::min(1.f, flow::micLevel() / 160.f);
        s.levelSmooth = s.levelSmooth * .7f + level * .3f;
        bob = -3.f * s.levelSmooth;
    }

    if (ui::home_eyes::isLoaded()) {
        // ── Home-size eyes: literally the Home renderer (same blink, motion, frames).
        if (t <= 0.f && c.mode != Mode::Reply && !s.eyesUp) {
            const bool textChanged = force || s.viewLayer != 1 || contentSig != s.viewContentSig;
            if (textChanged && !force && now - s.viewPaintMs < 33u) return;
            if (textChanged) {
                view::clear(T());
                ui::home_eyes::invalidatePaint();
            }
            const bool thinking = c.mode == Mode::Thinking;
            if (thinking) ui::home_eyes::drawThinkingEyes(T(), easeOut(age / 300.f), now, !textChanged);
            else ui::home_eyes::drawConversationEyes(T(), 0, bob, now, !textChanged);
            if (textChanged) {
                paintText(s, c);
                hal::display::markDirty(0, 240);
                s.viewLayer = 1; s.viewContentSig = contentSig; s.viewPaintMs = now;
            }
            s.transcriptPainted = s.transcriptRevision; s.replyPainted = s.replyRevision;
            return;
        }
        // ── Travelling between sizes: the cached travel frames, every frame.
        if (t < 1.f && (t > 0.f || c.mode == Mode::Reply || s.eyesUp)) {
            if (!force && now - s.viewPaintMs < 16u) return;
            const float a = t * kTravelEnd;
            view::clear(T());
            if (!ui::home_eyes::drawTravelEyes(T(), a, 102.f * a, travelOffsetY(a))) {
                view::EyeFrame e; e.t = t; view::eyes(T(), pose, e);
            }
            paintText(s, c);
            hal::display::markDirty(0, 240);
            s.viewLayer = 2; s.viewContentSig = contentSig; s.viewPaintMs = now;
            s.transcriptPainted = s.transcriptRevision; s.replyPainted = s.replyRevision;
            return;
        }
    }

    // ── Small resting eyes (reply) or no Home renderer: one full paint per change.
    const float open = blinkOpen(now);
    const float glance = c.mode == Mode::Thinking ? easeOut(age / 300.f) : 0.f;
    const uint32_t eyeSig = (uint32_t)(open * 40.f) | (uint32_t)(t * 100.f) << 8 | (uint32_t)(int)(bob * 4 + 64) << 16 |
                            (uint32_t)(glance * 20.f) << 24;
    const uint32_t sig = eyeSig * 31u ^ contentSig;
    if (!force && s.viewLayer == 3 && sig == s.viewSignature) return;
    if (!force && now - s.viewPaintMs < 16u) return;
    view::clear(T());
    if (ui::home_eyes::isLoaded()) {
        const int h = (int)(kSmallNominal * 91.f / 67.f * 1.3f) + 8;
        ui::eye_rig::drawRaster(T(), 0, (int)kSmallCenterY - h / 2, 280, h, pose, open, kUiBg, kSmallNominal,
                                3.f * glance, -3.f * glance + .5f * bob, false, 255, 0, -1, kSmallSpacing, 1);
    } else {
        view::EyeFrame e; e.t = t; e.open = open; e.offsetY = bob;
        if (c.mode == Mode::Thinking) { e.offsetX = 6 * glance; e.offsetY = -6 * glance; }
        view::eyes(T(), pose, e);
    }
    paintText(s, c);
    hal::display::markDirty(0, 240);
    s.viewLayer = 3; s.viewSignature = sig; s.viewContentSig = contentSig; s.viewPaintMs = now;
    s.transcriptPainted = s.transcriptRevision; s.replyPainted = s.replyRevision;
}

void paintReplyText(AgentState& s) { paintConversation(s, millis(), true); }

void paintMiniEyes(AgentState& s) {
    // Expression changed: repaint now so the new pose shows without waiting for text.
    s.miniEmotionRevision = emotion::snapshot(s.msgIndex).revision; s.miniEmotionSegment = s.msgIndex;
    paintConversation(s, millis(), true);
}

void beginRecordingView(AgentState& s, uint32_t nowMs) {
    motion::cancel();
    s.miniEyesOpen = true;
    hal::display::setFlushService(serviceMicCaptureEarly);
    s.transcriptPainted = UINT32_MAX;
    paintConversation(s, nowMs, true);
}

bool scrollListeningReply(AgentState& s, int delta) {
    if (s.handsFree && (s.continuous || !s.transcript[0]) && s.documentVisible && s.documentText) {
        const int maximum = std::max(0, view::count(s.documentText) - (s.continuous ? 2 : 4));
        const int next = std::max(0, std::min(maximum, s.documentScroll + delta));
        if (next == s.documentScroll) {
            const int offset = delta > 0 ? s.documentNextOffset : s.documentOffset > 0 ? std::max(0, s.documentOffset - 4095) : -1;
            if (offset >= 0 && !s.documentPagePending) { s.documentPagePending = true; services::agent_transport::requestDocumentWindow(offset); return true; }
            return false;
        }
        s.documentScroll = next;
        s.transcriptPainted = UINT32_MAX;
        return true;
    }
    // Agent mode scrolls the newest message, the one on screen.
    if (!delta || !s.continuous || !s.conv.anyNonEmpty()) return false;
    s.conv.refreshDisplay(std::max(0, s.conv.segmentCount() - 1));
    const int maximum = std::max(0, view::count(s.conv.displayText()) - (view::replyRows - 1));
    const int next = std::max(0, std::min(maximum, s.previousScrollLines + delta));
    s.listeningReplyFollowTail = delta > 0 && next == maximum;
    if (next == s.previousScrollLines) return false;
    s.previousScrollLines = next;
    s.transcriptPainted = UINT32_MAX;
    return true;
}

void paintRecordingBorder(AgentState& s, uint32_t nowMs, bool clear) {
    if (clear) { hal::display::setFlushService(nullptr); return; }
    paintConversation(s, nowMs, false);
}

void paintReplyShell(AgentState& s) {
    computeDialogueLayout(s);
    paintConversation(s, millis(), true);
}

// ─── Recording dot ───────────────────────────────────────────────────────────

void paintRecDot(AgentState& s, AgentState::RecDot mode, uint32_t nowMs) {
    (void)mode; (void)nowMs;
    s.recDotMode = AgentState::RecDot::Off;
}

// ─── Thinking shell ──────────────────────────────────────────────────────────

void paintThinkingShell(AgentState& s, uint8_t dotPhase) {
    s.thinkDotPhase = dotPhase % 3;
    paintConversation(s, millis(), false);
}

void paintEyesHomeStatic(AgentState& s) {
    const uint32_t paintStartUs = micros();
    releaseChatSprites(s);

    // Reset physics so eyes start centered on every entry to the home screen.
    s.eyeFloatX   = 0.f;
    s.eyeFloatY   = 0.f;
    ui::home_eyes::setEyeOffset(0, 0);

    // Clear AND invalidate every content row on scene entry. Raw sprite writes
    // alone leave menu/reply rows on the panel when only the eyes are flushed.
    // Do this before allocation/session setup so a failed setup is clean too.
    hal::display::clearContentArea(kUiBg);

    if (ui::home_eyes::isLoaded() && ui::home_eyes::isProcedural()) {
        ui::home_eyes::endSession();
    }
    hal::display::releaseSpiForSd();

    const uint32_t sessionStartUs = micros();
    const bool sessionReady = ui::home_eyes::beginSession(T());
    ui::home_eyes::setEmotion("neutral", true);
    Serial.printf("[boot-timing] event=home_eye_begin_session_return elapsed_us=%lu ready=%d\n",
                  (unsigned long)(micros() - sessionStartUs), sessionReady ? 1 : 0);
    if (sessionReady) {
        if (!ui::home_eyes::isProcedural() &&
            ui::home_eyes::numFrames() < HOME_SCREEN_NUM_FRAMES) {
            (void)ui::home_eyes::upgradeToFullSession(T());
        }
        if (ui::home_eyes::isProcedural()) {
            ui::home_eyes::resetProceduralPaintState(0);
        }
        const uint32_t drawStartUs = micros();
        ui::home_eyes::drawFrame(T(), 0, 0);
        Serial.printf("[boot-timing] event=home_eye_draw_ready draw_us=%lu total_paint_us=%lu\n",
                      (unsigned long)(micros() - drawStartUs),
                      (unsigned long)(micros() - paintStartUs));
    }
    ui::home_eyes::resetShellBlink();
    publishEyesRuntimeScene(s);
}

void serviceEyesShellTick(AgentState& s) {
    ui::home_eyes::setEyeOffset(s.eyeFloatX, s.eyeFloatY);
    ui::home_eyes::serviceEyesShell(T(), 0);
    static bool hadTimerPill=false;
    const auto timer=services::countdown::snapshot();
    if(timer.state==services::countdown::TimerState::Running || timer.state==services::countdown::TimerState::Paused) {
      ui::timer_controls::homePill(T(),timer.remaining);
      hal::display::markDirty(40,25);hadTimerPill=true;
    } else if(hadTimerPill) {
      T().fillRect(100,40,80,25,kUiBg);hal::display::markDirty(40,25);hadTimerPill=false;
    }
    publishEyesRuntimeScene(s);
}

void paintHomeConnStatus(int mode) {
    // Keep transport warmup state off the home screen; the eyes remain the only
    // persistent content there. Recording and connection behavior are unchanged.
    (void)mode;
}

void paintError(AgentState& s) {
    releaseChatSprites(s);
    ui::paintKamiNotice(T(), s.noticeKind, kSystemStatusBarH);
}

// ─── Reply reveal ────────────────────────────────────────────────────────────
//
// The reveal chases the received text instead of gating it: the first chunk
// paints one character at once, then text appears at a natural rate and never
// lags more than kRevealMaxLag characters behind what has arrived. Typing ticks
// play at most every kTypingCueMs while it moves, only when the microphone is
// closed and Kami is not speaking.

bool typewriterInProgress(const AgentState& s) {
    if (s.stage != Stage::Reply) return false;
    const char* text = s.conv.displayText();
    return text && s.twRevealLen < strlen(text);
}

void resetTypewriter(AgentState& s) {
    s.twRevealLen = 0;
    s.twLastMs    = millis();
}

void snapTypewriterToView(AgentState& s) {
    s.conv.refreshDisplay(s.msgIndex); s.twRevealLen = strlen(s.conv.displayText());
}

bool typewriterRevealed(AgentState& s) {
    if (s.stage != Stage::Reply) return false;
    s.conv.refreshDisplay(s.msgIndex);
    return s.twRevealLen >= strlen(s.conv.displayText());
}

static bool typingCueAllowed(const AgentState& s) {
    return !s.continuous && !services::agent_speech::enabled() && !services::agent_speech::busy() &&
           !services::agent_voice::isCapturing();
}

void serviceTypewriter(AgentState& s, uint32_t nowMs) {
    if (s.stage != Stage::Reply) return;
    s.conv.refreshDisplay(s.msgIndex);
    const char* full = s.conv.displayText();
    const size_t target = strlen(full);
    if (s.twRevealLen > target) s.twRevealLen = target;
    if (s.twRevealLen < target) {
        const uint32_t elapsed = nowMs - s.twLastMs;
        if (s.twRevealLen == 0 || elapsed >= cfg::agent::kRevealCharMs) {
            const size_t step = s.twRevealLen == 0 ? 1 : std::max<size_t>(1, elapsed / cfg::agent::kRevealCharMs);
            if (target - s.twRevealLen > cfg::agent::kRevealMaxLag) s.twRevealLen = target - cfg::agent::kRevealMaxLag;
            s.twRevealLen = std::min(target, s.twRevealLen + step);
            s.twLastMs = nowMs;
            if ((unsigned char)full[s.twRevealLen - 1] > ' ' && typingCueAllowed(s) &&
                nowMs - s.typingCueMs >= cfg::agent::kTypingCueMs &&
                sfx::product::play(sfx::product::Sound::Typing)) s.typingCueMs = nowMs;
        }
    }
    paintConversation(s, nowMs, false);
}

}  // namespace render
}  // namespace agent
}  // namespace apps
