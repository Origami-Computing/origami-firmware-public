#pragma once

#include <stddef.h>
#include <stdint.h>

#include "apps/agent/agent_notice.h"
#include "core/conversation_buffer.h"
#include "ui/agent_listening_border.h"

class TFT_eSprite;

namespace apps {
namespace agent {

/**
 * Lifecycle stage of the agent app. Each value corresponds to exactly one
 * screen — the screen's name documents what the user is seeing.
 *
 *   Eyes       → home screen with the animated Kami eyes
 *   Recording  → full-size Kami eyes + amplitude-driven red border (PTT held)
 *   Thinking   → "..." dialogue box (WebSocket agent turn in flight)
 *   Reply      → typewriter dialogue with the assistant text
 *   Error      → one of several error screens (see agent_config::kErr*)
 */
enum class Stage : uint8_t {
    Eyes      = 0,
    Recording = 1,
    Thinking  = 2,
    Reply     = 3,
    Error     = 4,
};

/**
 * Source of a "please cancel this in-flight send" request. The cancel is
 * raised from input handlers (Core 1) and consumed from inside the blocking
 * WebSocket worker.
 */
enum class CancelTarget : uint8_t {
    None  = 0,
    Menu  = 1,  // after cancel, navigate to the app menu
    Eyes  = 2,  // after cancel, return to the eyes home
};

/**
 * Single state struct that replaces ~30 file-static variables formerly at
 * the top of agent_app.cpp. Lives in app-module scope (one instance) and is
 * passed by reference into every screen callback.
 */
struct AgentState {
    // ── Lifecycle ────────────────────────────────────────────────────────────
    Stage stage             = Stage::Eyes;
    Stage prevStageForIdle  = Stage::Eyes;  // tracks transition for menu idle-pulse
    bool  pulseMenuActivity = false;
    bool  sendJustFinished  = false;

    // ── Buffers ──────────────────────────────────────────────────────────────
    core::ConversationBuffer conv;
    uint32_t voiceEntryMs = 0;
    uint32_t                pcmN = 0;   // captured sample count after stopRec()

    // ── Conversation reply layout (computed from cfg / ui_layout.h) ──────────
    int agentTextX     = 0;  // top-left of the dialogue *text* region (panel coords)
    int agentTextY     = 0;
    int agentTextW     = 0;  // width / height of the text region inside the box
    int agentTextH     = 0;
    int agentMaxLines  = 0;  // how many wrapped lines fit
    int agentLineH     = 0;  // px per line (PixelFont, scaled)

    // ── Reply screen sprites (live in PSRAM) ─────────────────────────────────
    TFT_eSprite* sprDialogue = nullptr;  // dialogue box content
    TFT_eSprite* sprMiniEye  = nullptr;  // small avatar above dialogue
    int          sprDialogueW = 0;
    int          sprDialogueH = 0;
    bool         spritesReady = false;

    // ── Typewriter / scroll state ────────────────────────────────────────────
    size_t   twRevealLen   = 0;
    uint32_t twLastMs      = 0;
    int      msgIndex      = 0;
    int      scrollLines   = 0;

    // ── Mini-eye blink ───────────────────────────────────────────────────────
    bool     miniEyesOpen = true;
    uint32_t miniBlinkMs  = 0;
    uint32_t miniBlinkStarted = 0;
    uint32_t miniEmotionRevision = 0;
    int miniEmotionSegment = -1;
    const char* miniEmotionName = "neutral";

    // ── Recording dot ────────────────────────────────────────────────────────
    enum class RecDot : uint8_t { Off, Grey, RedBlink };
    RecDot   recDotMode   = RecDot::Off;
    uint8_t  recDotPhase  = 255;  // last painted blink half-period

    // Recording visuals advance on UI ticks; never block microphone servicing.
    ui::agent_listening::Envelope recordingEnvelope;
    int recordingBorderThickness = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    int recordingLevelOverride = -1; // display-only diagnostic; never modifies PCM
#endif

    // ── Thinking dot phase ───────────────────────────────────────────────────
    uint8_t  thinkDotPhase = 0;
    char     thinkStatus[48] = {};

    // ── Chord-hold-to-record ─────────────────────────────────────────────────
    uint32_t chordHoldDeadlineMs = 0;
    bool     recStartedByChord  = false;

    // ── Send result handoff from kami_send task ──────────────────────────────
    CancelTarget cancelResult = CancelTarget::None;

    // ── Delegation handoff ───────────────────────────────────────────────────
    // A reply that promised later work (delegation) returns to the eyes once it
    // has been read; the result arrives as a pushed reply.
    bool     returnHomeAfterReply = false;
    uint32_t returnHomeAtMs       = 0;

    // ── Error message backing store ──────────────────────────────────────────
    char     errBuf[96] = {};
    notice::Kind noticeKind = notice::Kind::Service;
    uint32_t noticeSince = 0;
    notice::InputGate noticeInput;
    core::ConversationBuffer previousReply;
    bool hasPreviousReply = false, restoringReply = false, resumeNotice = false;
    int previousMsgIndex = 0, previousScrollLines = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    int wifiOverride = -1; // diagnostic gate only; never changes the radio
#endif

    // ── Animation cadence ────────────────────────────────────────────────────
    uint32_t animFrame = 0;
    uint32_t lastAnim  = 0;

    // ── Last displayed Home offset (transition handoff) ───────────────────
    float eyeFloatX    = 0.f;
    float eyeFloatY    = 0.f;

};

}  // namespace agent
}  // namespace apps
