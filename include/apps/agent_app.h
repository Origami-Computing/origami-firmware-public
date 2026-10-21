#pragma once
#include <stdint.h>
#include "app.h"

namespace apps {
namespace agent {

extern App app;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Local display fixtures; no recording, transport send, or persistent changes.
bool developmentUi(const char* command);
#endif

/** Reset the Home workflow before entering its view; used by lifecycle and flow. */
void prepareHome();
void exitApp();
/** Idle eyes without resetting mic/codec prep (global idle → home). */
void returnToHomescreen();
bool isAtHomescreen();
/** Idle with an assistant reply on screen (not the eyes home). */
bool isOnReplyScreen();
/** True on the static red-X error frame (WiFi / mic / transport failures). */
bool isShowingError();

/** Scroll agent reply down by one wrapped line (legacy tap hook). */
void scrollConversationDown();

/** Reply view: scroll agent text by wrapped lines (SCROLL_UP/DOWN). Returns true if handled. */
bool scrollReplyWithRotary(int lineDelta);

/**
 * Reply view: A steps to the next assistant segment or text page; when there is no further content,
 * A returns to the eyes home (same as finishing reading). B still returns home from any reply page.
 * Rotary scrolls lines, then moves to the next / previous segment at the bottom / top.
 */
bool advanceReplyPage();

/** Open Menu from the root; otherwise pop one app, preserving legacy exit behavior. */
void requestExitToMenu();

/** True while the reply view is animating letter-by-letter for the current wrapped window. */
bool isConversationTyping();

/** True between long-press record start and release (menu uses for PTT edge routing). */
bool isRecording();

/** Composite the listening border after system chrome (only while recording). */
void drawRecordingOverlay();

/** Smoothed mic loudness while recording, 0..255 (drives the recording edge glow). */
uint8_t recordingLevel();

/** True while the WebSocket agent turn for the current utterance is in progress. */
bool isSending();

/**
 * Request exiting to the app list while a reply is still streaming. Cooperative polling inside the
 * WebSocket worker handles most navigation; this lets the chord handler latch an escape before the
 * next network tick.
 */
void requestAbortSendingToMenu();
void requestAbortSendingToEyes();

/** Menu idle: consume one-shot "user just finished a voice interaction" to extend the global deadline. */
bool takeMenuActivityPulse();

/** True once after a send completes and transitions to conversation. Menu resyncs button state. */
bool takeSendJustFinished();

}  // namespace agent
}  // namespace apps

namespace apps { namespace agent { void serviceMicCaptureEarly(); } }
