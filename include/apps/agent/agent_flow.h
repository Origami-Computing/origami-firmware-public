#pragma once

#include <stdint.h>

#include "apps/agent/agent_state.h"

/**
 * Inter-screen flow primitives.
 *
 * Screens don't directly mutate `state.stage` — they call these helpers. That
 * way the central coordinator gets a chance to run onExit/onEnter, log the
 * transition, and reset shared state.
 */
namespace apps {
namespace agent {
namespace flow {

/** Get a reference to the singleton AgentState. */
AgentState& state();

/** Transition to the given stage. Runs onExit/onEnter and repaints if needed.
 *  No-op if already in `next`. */
void goTo(Stage next);

/** Start the recording flow. Acquires PSRAM, starts mic, transitions to Recording.
 *  On any failure, fills state.errBuf and transitions to Error. */
void startRecording();
bool wifiReady();
void dismissNotice();
void openNoticeSettings();
void serviceNotice(uint32_t now);

/** Stop the recording flow. Stops mic, transitions to Thinking. */
void finishRecording();

/** Start the persistent WebSocket transport task. Idempotent. */
bool ensureTransport(bool reconnectIfOffline = false);

/** Validate preconditions and commit the already-streaming recording.
 *  Non-blocking: ui_task continues to run normally.
 *  On precondition failure, transitions to Error immediately on ui_task. */
void startSend();

/** True when the WebSocket task is connected and past the short stability gate. */
bool isTransportReady();

/** True while a recording/send session is active in the transport worker. */
bool isSessionActive();

/** True after release/commit request while waiting for the transport worker. */
bool isCommitPending();

/** True once the commit frame reached the WebSocket transport. */
bool isCommitSent();

/** UI-task response policy, called after painting the visible Thinking view.
 *  Prioritizes first reply, then completion, then the existing watchdog.
 *  visibleSinceMs is the view-entry timestamp: overlay return intentionally
 *  restarts this grace period. Does not change transport cancellation policy. */
void pollThinking(uint32_t visibleSinceMs);

/** UI task: consume a ready result and apply its workflow transition. Returns
 *  true when completion was handled, false when no result was ready. Called
 *  before Reply animation; Thinking prioritizes first reply before completion. */
bool pollCompletion();

/** Signal the transport to abort the in-flight request and navigate to
 *  `nav` when the abort completes. Call from the UI task. */
void cancelSend(CancelTarget nav);

/** Return to the eyes home, clearing reply state and freeing slab+sprites. */
void returnToHome();

/** UI task, Eyes only: show the oldest reply that arrived outside a turn
 *  (a delegation result). Returns true when it switched to Reply. */
bool showInboxReply();
void awaitDelegatedReplies(uint8_t count);
bool waitingForDelegation();
void cancelDelegation();
void pollDelegation(uint32_t now);
void acknowledgeDisplayed();

/** UI task, Reply only: after a reply that handed work off, return to the
 *  eyes a short while after the last segment has been revealed. */
void pollReturnHomeAfterReply(uint32_t nowMs, bool revealed);

/** UI task: the user interacted with the reply; keep it on screen. */
void keepReply();

/** Latest shared 100ms microphone peak, 0..255. */
uint8_t micLevel();

}  // namespace flow
}  // namespace agent
}  // namespace apps
