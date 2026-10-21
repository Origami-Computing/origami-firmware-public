#include "services/agent_latency.h"
#include "ui/default_font_metrics.h"
#include "apps/agent/agent_motion.h"
#include "services/agent_voice.h"
#include "apps/agent/agent_render.h"

#include <Arduino.h>
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

using ui::text::kPokeCW;
static constexpr int kPokeLH=ui::kami_dialogue::kLineH;
using ui::text::kPokeSz;

// ─── Layout constants derived from ui_layout.h ──────────────────────────────
//
// The dialogue box outer rectangle is fixed (kDialogueX/Y/W/H). Inside it we
// inset by the border + padding to get the text region (kDlgTextX/Y/W/H).
// First-line baseline sits kPokeAscent px below the inner top edge because
// PixelFont's cursor y is the baseline.

static constexpr int kPokeAscent = 7 * kPokeSz;
static constexpr int kDlgTextX = kDialogueBorderW + kDialoguePadX;
static constexpr int kDlgInsetY = kDialogueBorderW + kDialoguePadY;
static constexpr int kDlgTextY = kDlgInsetY + kPokeAscent;
static constexpr int kDlgTextW = kDialogueW - 2 * kDlgTextX;
static constexpr int kDlgTextH = kDialogueH - 2 * kDlgInsetY;

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

void buildRecordingScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("recording.bg", {0,0,width,height}, TFT_RED, 0, 0);
    constexpr int inset = ui::agent_listening::kMinThickness;
    scene.addRect("listening.window", {inset,inset,(int16_t)(width-2*inset),
                  (int16_t)(height-2*inset)}, kUiBg, ui::agent_listening::kRadius-inset, 1);
    scene.addBitmap("listening.eyes", {42,57,196,126}, assets::kBootKamiEyesAlpha4,
                    assets::kBootKamiEyesW,assets::kBootKamiEyesH,kUiFg,kUiBg,2,4);
}

void buildThinkingScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("thinking.bg", {0, kSystemStatusBarH, width,
                                  (int16_t)(height - kSystemStatusBarH)},
                  kUiBg, 0, 0);
    scene.addLabel("thinking.label", {0, agent_layout::kThinkingY, width, 16},
                   "...", kUiFg, kUiBg, 2, ui::scene::TextAlign::TopCenter, 2);
}

void buildReplyScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    scene.addRect("reply.bg", {0, kSystemStatusBarH, width,
                               (int16_t)(height - kSystemStatusBarH)},
                  kUiBg, 0, 0);
    scene.addBitmap("mini.eye", {kMiniEyeX,kMiniEyeY,kMiniEyeW,kMiniEyeH},
                    assets::kami_dialogue::kBody,24,26,0xF945,kUiBg,1,4);
    scene.addBitmap("mini.pupils", {kMiniEyeX+4,kMiniEyeY+7,16,10},
                    assets::kami_dialogue::kEyes,16,10,kUiFg,0xF945,2,4);
    scene.addRect("dialogue.box", {kDialogueX, kDialogueY, kDialogueW, kDialogueH},
                  kUiDialogueFill, kDialogueCornerR, 1);
    scene.addRect("dialogue.padding",
                  {(int16_t)(kDialogueX + kDialoguePadX),
                   (int16_t)(kDialogueY + kDialoguePadY),
                   (int16_t)(kDialogueW - 2 * kDialoguePadX),
                   (int16_t)(kDialogueH - 2 * kDialoguePadY)},
                  kUiDialogueFill, 0, 2);
    scene.addLabel("dialogue.scroll.hint",
                   {(int16_t)(kDialogueX + kDialogueW - kDialogueScrollHintInsetX),
                    (int16_t)(kDialogueY + kDialogueH - 18), 12, 12},
                   "v", kUiMuted, kUiDialogueFill, 1,
                   ui::scene::TextAlign::TopLeft, 3);
}

void buildErrorScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    const auto kind=flow::state().noticeKind;
    scene.addRect("error.bg", {0,kSystemStatusBarH,width,(int16_t)(height-kSystemStatusBarH)},kUiBg,0,0);
    scene.addLabel("error.text",{36,90,208,110},notice::message(kind),kUiFg,kUiBg,2,ui::scene::TextAlign::TopLeft,1);
    scene.addLabel("error.footer",{36,224,208,12},notice::needsWifi(kind)?"A Back   B Wi-Fi":kind==notice::Kind::Auth?"A Back   B Settings":"A Back   A+B Talk",kUiFg,kUiBg,1,ui::scene::TextAlign::TopLeft,1);
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

// ─── Internal: text-wrap thin wrappers ──────────────────────────────────────

static int countWrappedLines(const char* s, int maxW) {
    return ui::text::countWrapped(s, maxW, 2);
}

static const char* skipWrappedLines(const char* s, int skip, int maxW) {
    return ui::text::skipWrapped(s, skip, maxW, 2);
}

static size_t visibleWrappedWindowLen(const char* s, int maxLines, int maxW) {
    if (!s || !*s || maxLines <= 0 || maxW <= 0) return 0;
    const char* next = ui::text::skipWrapped(s, maxLines, maxW, 2);
    if (!next || next < s) return strlen(s);
    return (size_t)(next - s);
}

static int drawWrappedPokeTextPrefix(TFT_eSPI& d, AgentState& s, const char* text,
                                     size_t pc, int x, int y, int maxW, int maxLines) {
    return ui::text::wrappedPokemonPrefix(d, text, pc, x, y, maxW, maxLines,
                                          kUiFg, kUiDialogueFill,
                                          s.conv.wrapScratch(), s.conv.wrapScratchSize(), kPokeLH);
}

static void flushDialogueSprite(AgentState& s) {
    if (!s.spritesReady || !s.sprDialogue || !s.sprDialogue->created()) return;
    s.sprDialogue->pushSprite(kDialogueX, kDialogueY);
    hal::display::markDirty(kDialogueY, s.sprDialogueH);
}

// ─── Layout ──────────────────────────────────────────────────────────────────

void computeDialogueLayout(AgentState& s) {
    s.agentTextX    = kDialogueX + kDlgTextX;
    s.agentTextY    = kDialogueY + kDlgTextY;
    s.agentTextW    = kDlgTextW;
    s.agentTextH    = kDlgTextH;
    // Include the selected face's complete cell, including descenders.
#ifdef ORIGAMI_INSTRUMENT_FONT
    s.agentMaxLines = (kDlgTextH - ui::default_font::height(2)) / kPokeLH + 1;
#else
    s.agentMaxLines = (kDlgTextH - 18) / kPokeLH + 1;
#endif
    if (s.agentMaxLines < 2)  s.agentMaxLines = 2;
    if (s.agentMaxLines > 48) s.agentMaxLines = 48;
    s.agentLineH    = kPokeLH;
}

// ─── Sprite lifecycle ────────────────────────────────────────────────────────

bool ensureChatSprites(AgentState& s) {
    if (s.agentTextW <= 0 || s.agentTextH <= 0) return false;

    const int  sprW = kDialogueW;
    const int  sprH = kDialogueH;
    const bool hasDialogue =
        s.sprDialogue && s.sprDialogue->created() &&
        s.sprDialogueW == sprW && s.sprDialogueH == sprH;
    if (s.spritesReady && hasDialogue) return true;

    releaseChatSprites(s);

    s.sprDialogue = core::memory::allocSprite("agent.dialogue", T(), sprW, sprH, 16);
    if (!s.sprDialogue) return false;
    s.sprDialogueW = sprW;
    s.sprDialogueH = sprH;

    // The small native avatar paints directly without a sprite or SD read.

    s.spritesReady = true;
    return true;
}

void releaseChatSprites(AgentState& s) {
    s.spritesReady = false;
    core::memory::freeSprite(s.sprMiniEye);
    core::memory::freeSprite(s.sprDialogue);
    s.sprDialogueW = s.sprDialogueH = 0;
}

// ─── Reply text (typewriter) ─────────────────────────────────────────────────

static void paintReplyTextInner(AgentState& s) {
    if (s.agentTextW <= 0 || s.agentTextH <= 0) return;

    s.conv.refreshDisplay(s.msgIndex);
    const char* view = skipWrappedLines(s.conv.displayText(), s.scrollLines, s.agentTextW);
    const size_t vl  = visibleWrappedWindowLen(view, s.agentMaxLines, s.agentTextW);
    size_t pc        = s.twRevealLen;
    if (pc > vl) pc = vl;

    const int  totalLines = countWrappedLines(s.conv.displayText(), s.agentTextW);
    const bool moreBelow  = (s.scrollLines + s.agentMaxLines) < totalLines;

    if (s.spritesReady && s.sprDialogue && s.sprDialogue->created()) {
        s.sprDialogue->fillSprite(kUiDialogueFill);
        ui::dialogue::drawFrame(*s.sprDialogue, kDialogueW, kDialogueH);
        drawWrappedPokeTextPrefix(*s.sprDialogue, s, view ? view : "", pc,
                                  kDlgTextX, kDlgTextY, kDlgTextW, s.agentMaxLines);
        if (moreBelow) {
            const int lastBaseline = kDlgTextY + (s.agentMaxLines - 1) * kPokeLH;
            const int chevronX     = kDialogueW - kDialogueScrollHintInsetX;
            const int chevronY     = lastBaseline + kDialogueScrollHintGapY;
            ui::dialogue::drawScrollHint(*s.sprDialogue, chevronX, chevronY);
        }
    } else {
        ui::dialogue::drawFrameDirect(T(), kDialogueX, kDialogueY, kDialogueW, kDialogueH);
        drawWrappedPokeTextPrefix(T(), s, view ? view : "", pc,
                                  kDialogueX + kDlgTextX, kDialogueY + kDlgTextY,
                                  kDlgTextW, s.agentMaxLines);
        if (moreBelow) {
            const int x=kDialogueX+kDialogueW-kDialogueScrollHintInsetX;
            const int y=kDialogueY+kDlgTextY+(s.agentMaxLines-1)*kPokeLH+kDialogueScrollHintGapY;
            T().fillTriangle(x,y,x+6,y,x+3,y+3,kUiDialogueHint);
        }
    }
    // Arm only after at least one non-whitespace reply character was painted.
    // Completion is recorded by HAL after the dirty text band reaches SPI.
    for(size_t i=0;view && i<pc;++i)if((unsigned char)view[i]>' '){
        services::agent_latency::painted(kDialogueY,kDialogueY+kDialogueH);break;
    }

}

void paintReplyText(AgentState& s) {
    paintReplyTextInner(s);
    if (s.spritesReady) flushDialogueSprite(s);
}

// ─── Mini eyes ───────────────────────────────────────────────────────────────
static ui::eye_rig::Pose miniPose=ui::eye_rig::kPresets[0].pose,miniFrom=miniPose;
static uint32_t miniPoseStarted=0;
static bool miniPoseReady=false;
static ui::kami_expression::Shape miniShape=ui::kami_expression::Shape::Open;


void paintMiniEyes(AgentState& s) {
    const auto selected=s.stage==Stage::Reply?emotion::snapshot(s.msgIndex):emotion::Selection{};
    const uint32_t now=millis();
    if(!miniPoseReady){miniPose=miniFrom=selected.pose;miniShape=selected.shape;miniPoseReady=true;miniPoseStarted=now;}
    if(s.miniEmotionRevision!=selected.revision || s.miniEmotionSegment!=s.msgIndex){
      miniFrom=miniPose;miniPoseStarted=now;
    }
    miniPose=ui::eye_rig::interpolate(miniFrom,selected.pose,
        ui::agent_motion::ramp(now-miniPoseStarted,0,280));
    // Named shapes switch while the eyes are already closed, never by a flash
    // during an otherwise open pose. Ordinary lids ease continuously.
    if(!s.miniEyesOpen && now-s.miniBlinkStarted>=50 && now-s.miniBlinkStarted<130)
      miniShape=selected.shape;
    auto frame=s.miniEyesOpen?ui::kami_expression::Frame{}:ui::kami_expression::blink(now-s.miniBlinkStarted);
    if(frame.shape==ui::kami_expression::Shape::Open){frame.shape=miniShape;
      if(frame.shape==ui::kami_expression::Shape::Spiral)frame.spinMs=now%2078700u;
      if(frame.shape==ui::kami_expression::Shape::Bowl)frame.zzzMs=now%3400u;}
    ui::kami_dialogue::drawAvatarPose(T(),kMiniEyeX,kMiniEyeY,s.miniEyesOpen,miniPose,ui::eye_rig::kDefaultSpacing,&frame);
    if(s.miniEmotionRevision!=selected.revision||s.miniEmotionSegment!=s.msgIndex||strcmp(s.miniEmotionName,selected.name))
        Serial.printf("[agent-emotion] rendered=%s segment=%d avatar=24x26\n",selected.name,s.msgIndex);
    s.miniEmotionRevision=selected.revision;s.miniEmotionSegment=s.msgIndex;s.miniEmotionName=selected.name;
    hal::display::markDirty(kMiniEyeY,kMiniEyeH);
}

void beginRecordingView(AgentState& s, uint32_t nowMs) {
    s.recordingEnvelope.reset(nowMs);
    s.recordingBorderThickness = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    s.recordingLevelOverride = -1;
#endif
    ui::home_eyes::setEyeOffset(0,0);
    motion::startListening(nowMs,s.eyeFloatX,s.eyeFloatY);
    // NavHandler draws the recording overlay before startRecording() flushes.
    // Drawing here too repeats the expensive first frame before it is visible.
}

void paintRecordingBorder(AgentState& s, uint32_t nowMs, bool clear) {
    // Keep the border base until Thinking has faded the listening scene.
    if (!clear) motion::drawListening(s,nowMs);
}

void paintReplyShell(AgentState& s) {
    miniPoseReady=false;
    computeDialogueLayout(s);

    s.conv.refreshDisplay(s.msgIndex);

    hal::display::clearContentArea(kUiBg);
    hal::display::releaseSpiForSd();

    paintMiniEyes(s);

    if (ensureChatSprites(s)) {
        paintReplyTextInner(s);
        s.sprDialogue->pushSprite(kDialogueX, kDialogueY);
    } else {
        Serial.println("[agent] shell: sprite alloc FAILED, drawing direct to TFT");
        paintReplyTextInner(s);
    }
}

// ─── Recording dot ───────────────────────────────────────────────────────────

void paintRecDot(AgentState& s, AgentState::RecDot mode, uint32_t nowMs) {
    using cfg::agent::kRecBlinkOnMs;
    using cfg::agent::kRecBlinkPeriodMs;
    const int radius = agent_layout::kRecDotRadius;
    const int cx   = agent_layout::kRecDotX;
    const int cy   = agent_layout::kRecDotY;
    const int wipe = radius * 2 + 4;

    if (mode == AgentState::RecDot::Off) {
        if (s.recDotMode != AgentState::RecDot::Off) {
            T().fillRect(cx - radius - 1, cy - radius - 1, wipe, wipe, kUiBg);
        }
        s.recDotMode  = AgentState::RecDot::Off;
        s.recDotPhase = 255;
        return;
    }

    uint8_t targetPh = 254;  // solid (grey)
    if (mode == AgentState::RecDot::RedBlink) {
        const bool lit = ((nowMs % kRecBlinkPeriodMs) < kRecBlinkOnMs);
        targetPh = lit ? 1u : 0u;
    }

    if (mode == s.recDotMode && targetPh == s.recDotPhase) return;

    T().fillRect(cx - radius - 1, cy - radius - 1, wipe, wipe, kUiBg);
    s.recDotMode  = mode;
    s.recDotPhase = targetPh;

    if (mode == AgentState::RecDot::RedBlink && targetPh == 0u) return;  // off half

    const uint16_t fill = (mode == AgentState::RecDot::Grey)
                              ? agent_layout::kRecDotIdleColor
                              : agent_layout::kRecDotActiveColor;
    T().fillCircle(cx, cy, radius, fill);
}

// ─── Thinking shell ──────────────────────────────────────────────────────────

void paintThinkingShell(AgentState& s, uint8_t dotPhase) {
    s.thinkDotPhase=dotPhase%3;
    motion::drawThinking(millis());
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

// ─── Typewriter ──────────────────────────────────────────────────────────────

bool typewriterInProgress(const AgentState& s) {
    if (s.stage != Stage::Reply || s.agentTextW <= 0) return false;
    const char* view = skipWrappedLines(s.conv.displayText(), s.scrollLines, s.agentTextW);
    const size_t vl = visibleWrappedWindowLen(view, s.agentMaxLines, s.agentTextW);
    return view && view[0] && s.twRevealLen < vl;
}

void resetTypewriter(AgentState& s) {
    s.twRevealLen = 0;
    s.twLastMs    = millis() - cfg::agent::kCharIntervalMs;
}

void snapTypewriterToView(AgentState& s) {
    s.conv.refreshDisplay(s.msgIndex);
    const char* view = skipWrappedLines(s.conv.displayText(), s.scrollLines, s.agentTextW);
    s.twRevealLen = visibleWrappedWindowLen(view, s.agentMaxLines, s.agentTextW);
    s.twLastMs    = millis();
}

bool typewriterRevealed(AgentState& s) {
    if (s.stage != Stage::Reply || s.agentTextW <= 0) return false;
    s.conv.refreshDisplay(s.msgIndex);
    const char* view = skipWrappedLines(s.conv.displayText(), s.scrollLines, s.agentTextW);
    return s.twRevealLen >= visibleWrappedWindowLen(view, s.agentMaxLines, s.agentTextW);
}

void serviceTypewriter(AgentState& s, uint32_t nowMs) {
    using cfg::agent::kCharIntervalMs;
    using cfg::agent::kFastBurstMax;
    using cfg::agent::kFastBurstThreshold;

    if (s.stage != Stage::Reply || s.agentTextW <= 0) return;
    s.conv.refreshDisplay(s.msgIndex);
    const char* view = skipWrappedLines(s.conv.displayText(), s.scrollLines, s.agentTextW);
    const size_t vl  = visibleWrappedWindowLen(view, s.agentMaxLines, s.agentTextW);
    if (vl == 0 || s.twRevealLen >= vl) return;
    if ((uint32_t)(nowMs - s.twLastMs) < kCharIntervalMs) return;
    s.twLastMs = nowMs;

    size_t remaining = vl - s.twRevealLen;
    size_t burst     = 1;
    if (vl >= kFastBurstThreshold && remaining > 0) {
        burst = remaining / 24u;
        if (burst < 2u)               burst = 2u;
        if (burst > kFastBurstMax)    burst = kFastBurstMax;
        if (burst > remaining)        burst = remaining;
    }

    char lastRevealed = ' ';
    for (size_t n = 0; n < burst && s.twRevealLen < vl; n++) {
        lastRevealed = view[s.twRevealLen];
        s.twRevealLen++;
    }
    sfx::typewriterServiceChar(lastRevealed, s.twRevealLen - 1u);
    paintReplyText(s);
}

}  // namespace render
}  // namespace agent
}  // namespace apps
