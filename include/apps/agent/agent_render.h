#pragma once

#include <stdint.h>

#include "apps/agent/agent_state.h"

namespace ui {
namespace scene {
class Scene;
}
}

/**
 * Cross-screen render helpers for the agent app.
 *
 * Each function takes the AgentState and reads only the layout / sprite / text
 * fields. They do not change `stage`, `pcmN`, network flags, etc.
 *
 * Logically split into three groups:
 *   - layout()           — compute the dialogue rect from cfg + ui_layout
 *   - sprite()           — alloc/free the dialogue and mini-eye PSRAM sprites
 *   - paint()            — actually push pixels to the panel
 */
namespace apps {
namespace agent {
namespace render {

// ── Layout ───────────────────────────────────────────────────────────────────

/** Populate the agentTextX/Y/W/H + agentMaxLines + agentLineH fields. */
void computeDialogueLayout(AgentState& s);

// ── Sprite lifecycle ─────────────────────────────────────────────────────────

/** Free any allocated chat sprites. Idempotent. */
void releaseChatSprites(AgentState& s);

// ── Per-screen paints ────────────────────────────────────────────────────────

/** Paint the full-panel conversation view (eyes, your line, Kami's reply).
 * Skips unchanged frames unless force; UI task only. */
void paintConversation(AgentState& s, uint32_t nowMs, bool force);

/** Paint available reply text on the open conversation surface. */
void paintReplyText(AgentState& s);

/** Full repaint of the open conversation surface. */
void paintReplyShell(AgentState& s);

/** Keep canonical eyes above the live transcript while capturing. */
void beginRecordingView(AgentState& s, uint32_t nowMs);
/** Scroll the retained answer while listening. Scrolling back suspends live
 * tail-follow; reaching the bottom resumes it for subsequent reply deltas. */
bool scrollListeningReply(AgentState& s, int delta);
void paintRecordingBorder(AgentState& s, uint32_t nowMs, bool clear = false);

/** Paint canonical conversation eyes on expression/blink changes. */
void paintMiniEyes(AgentState& s);

/** Paint the recording dot in the bottom-right corner with the given mode. */
void paintRecDot(AgentState& s, AgentState::RecDot mode, uint32_t nowMs);

/** Keep the latest transcript visible while awaiting the first reply. */
void paintThinkingShell(AgentState& s, uint8_t dotPhase);

/** Paint the homescreen eyes (lazy-load eye sprites from SD if needed). */
void paintEyesHomeStatic(AgentState& s);

/** Service one animation step of the eye shell (cheap, called on idle ticks). */
void serviceEyesShellTick(AgentState& s);

/** Bottom-of-screen status hint.
 *  mode: 0 = hide, 1 = "Connecting…", 2 = "Connected". */
void paintHomeConnStatus(int mode);

/** Paint the appropriate error screen using state.errBuf. */
void paintError(AgentState& s);

// ── Side-effect-free editor scenes ──────────────────────────────────────────

void buildEyesScene(ui::scene::Scene& scene, int16_t width, int16_t height);
void buildRecordingScene(ui::scene::Scene& scene, int16_t width, int16_t height);
void buildThinkingScene(ui::scene::Scene& scene, int16_t width, int16_t height);
void buildReplyScene(ui::scene::Scene& scene, int16_t width, int16_t height);
void buildErrorScene(ui::scene::Scene& scene, int16_t width, int16_t height);

// ── Typewriter ───────────────────────────────────────────────────────────────

/** Advance the chasing reply reveal (typing ticks when the mic is closed) and repaint. */
void serviceTypewriter(AgentState& s, uint32_t nowMs);

/** True once the visible window of the current segment is fully revealed. */
bool typewriterRevealed(AgentState& s);

/** Reset compatibility reveal bookkeeping for a new reply. */
void resetTypewriter(AgentState& s);

/** Jump the reveal to the end of the current view (after scroll). */
void snapTypewriterToView(AgentState& s);

/** True while there are still chars to reveal in the current view. */
bool typewriterInProgress(const AgentState& s);

}  // namespace render
}  // namespace agent
}  // namespace apps
