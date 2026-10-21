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

/** Ensure the dialogue + mini-eye sprites are allocated. Returns true on success. */
bool ensureChatSprites(AgentState& s);

/** Free any allocated chat sprites. Idempotent. */
void releaseChatSprites(AgentState& s);

// ── Per-screen paints ────────────────────────────────────────────────────────

/** Paint just the dialogue-text contents (assumes shell + frame already drawn). */
void paintReplyText(AgentState& s);

/** Full repaint of the reply screen (mini-eye + dialogue + text). */
void paintReplyShell(AgentState& s);

/** Keep full-size eyes while capturing, with a red amplitude-driven border. */
void beginRecordingView(AgentState& s, uint32_t nowMs);
void paintRecordingBorder(AgentState& s, uint32_t nowMs, bool clear = false);

/** Paint just the mini-eye avatar (cheap, called on blink). */
void paintMiniEyes(AgentState& s);

/** Paint the recording dot in the bottom-right corner with the given mode. */
void paintRecDot(AgentState& s, AgentState::RecDot mode, uint32_t nowMs);

/** Paint the thinking dialogue with "." / ".." / "..." centered (chosen by phase). */
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

/** Advance the typewriter reveal by 1 or more chars (per cfg::agent::kCharIntervalMs). */
void serviceTypewriter(AgentState& s, uint32_t nowMs);

/** True once the visible window of the current segment is fully revealed. */
bool typewriterRevealed(AgentState& s);

/** Reset reveal to zero so the next service-tick starts from the first char. */
void resetTypewriter(AgentState& s);

/** Jump the reveal to the end of the current view (after scroll). */
void snapTypewriterToView(AgentState& s);

/** True while there are still chars to reveal in the current view. */
bool typewriterInProgress(const AgentState& s);

}  // namespace render
}  // namespace agent
}  // namespace apps
