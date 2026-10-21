#pragma once

#include <stdint.h>
#include "services/agent_transport_config.h"

/**
 * Agent presentation constants; transport policy is included from services.
 *
 * If you want to change how Kami looks or feels, this is the file to edit.
 * Layout pixel sizes live in `config/ui_layout.h` (so other apps can share
 * the same dialogue box geometry); animation timing, input semantics and
 * per-screen messages live here. Transport constants live in
 * `services/agent_transport_config.h`.
 *
 * Naming convention:
 *   kFooMs       → milliseconds
 *   kFooPx       → pixels
 *   kFooCount    → integer counts
 *   kFoo         → all-other constants
 */
namespace cfg {
namespace agent {

// ─── Typewriter (Screen: reply) ──────────────────────────────────────────────

/** Milliseconds between revealing successive characters in the reply screen. */
constexpr uint32_t kCharIntervalMs = 1;

/** Reply length above which the typewriter reveals chars in bursts (not 1 at a time). */
constexpr uint32_t kFastBurstThreshold = 24;

/** Maximum chars revealed per tick when in fast-burst mode. */
constexpr uint32_t kFastBurstMax = 32;

// ─── Thinking dots (Screen: thinking) ────────────────────────────────────────

/** Period for one step of "." / ".." / "..." cycling. */
constexpr uint32_t kDotStepMs = 300;

/** Minimum delay before triggering the actual HTTP POST — gives the user a beat
 *  of "..." animation before Core 1 blocks. */
constexpr uint32_t kPreSendAnimMs = 400;

// ─── Recording dot (Screen: eyes during PTT) ─────────────────────────────────

/** Recording dot blink period (full cycle, on + off). */
constexpr uint32_t kRecBlinkPeriodMs = 260;

/** Of the period, how many milliseconds the dot is lit. */
constexpr uint32_t kRecBlinkOnMs = 210;

/** Dot radius in pixels. */
constexpr int kRecDotRadiusPx = 8;

// ─── Mini-eye blink (Screen: reply, top-left avatar) ─────────────────────────

/** How long the mini-eyes stay closed during a blink. */
constexpr uint32_t kMiniBlinkClosedMs = 120;

/** Lower bound of random open interval between blinks. */
constexpr uint32_t kMiniBlinkMinOpenMs = 2500;

/** Upper bound of random open interval between blinks. */
constexpr uint32_t kMiniBlinkMaxOpenMs = 5000;

// ─── Idle UI cadence ─────────────────────────────────────────────────────────

/** Eye-shell animation tick rate on the home screen (lower = smoother). */
constexpr uint32_t kIdleEyesPeriodMs = 32;

/** Reply screen tick rate (chrome only — typewriter has its own cadence). */
constexpr uint32_t kIdleReplyPeriodMs = 72;

// ─── Input / chord semantics ─────────────────────────────────────────────────

/** A+B chord must be held this long after the chord event before it upgrades
 *  into a PTT recording. Shorter = "A+B tap" returns to eyes only. */
constexpr uint32_t kChordHoldRecordMs = 200;

// ─── Network ─────────────────────────────────────────────────────────────────


// ─── Errors (Screen: error) ──────────────────────────────────────────────────

/** Error string when Wi-Fi is not connected. */
constexpr const char* kErrWifi = "WiFi not connected";

/** Error when the PSRAM audio ring acquire fails after fallback attempts. */
constexpr const char* kErrPsram = "Audio memory unavailable";

/** Error when background services cannot yield voice-session resources promptly. */
constexpr const char* kErrResourcesBusy = "System busy - retry";

/** Error when MicCapture::start() fails. */
constexpr const char* kErrMic = "Mic init failed";

/** Error when no audio was captured before release. */
constexpr const char* kErrNoAudio = "No audio recorded";

/** Error when local audio backlog fills before the WebSocket can drain it. */
constexpr const char* kErrAudioOverflow = "Audio buffer overflow - network slow";

/** Error when the ConversationBuffer cannot be allocated. */
constexpr const char* kErrConvBuf = "PSRAM text buffers";

/** Error when the WebSocket response contained no usable segments. */
constexpr const char* kErrEmptyReply = "Empty reply";

/** Error when the reply UI couldn't be allocated after a successful send. */
constexpr const char* kErrReplyUi = "Reply UI memory";

// After a reply that handed work to a delegated task, the eyes return this
// long after the last segment is fully revealed; the result arrives later.
constexpr uint32_t kReturnHomeAfterHandoffMs = 3000;

}  // namespace agent
}  // namespace cfg
