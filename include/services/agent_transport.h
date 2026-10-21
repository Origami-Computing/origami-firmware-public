#pragma once
#include <stdint.h>

namespace services { namespace agent_transport {
enum class Outcome : uint8_t { Ok, HttpError, NetworkError, Cancelled };
struct Result { Outcome outcome = Outcome::NetworkError; uint8_t cancelTag = 0; uint8_t pendingTasks = 0; };

// Immutable, non-null callbacks with static lifetime, installed before a voice turn.
// PCM access occurs under the audio lock on the worker and microphone producer.
// PCM/capacity callbacks must be nonblocking pointer/size reads. The owner calls
// detachAudio() before freeing or replacing their storage. All reply
// callbacks run only from pollReplies() on the UI task. Completion runs before
// done() is published. Reply callbacks must not navigate or draw.
struct VoiceCallbacks {
    int16_t* (*pcm)();
    uint32_t (*capacity)();
    bool (*message)(const char* text, bool append);
    bool (*hasReply)();
    void (*status)(const char* text);
    Outcome (*finish)(Outcome outcome, const char* error, uint32_t sentSamples);
    void (*event)(const char* json); // Ordered foreground metadata, UI task only.
    void (*discard)(int segment);    // Remove a streamed draft the server withdrew.
};
// Call every UI pass, before reading reply progress or rendering.
void pollReplies();
// UI task: optionally kick the existing inline STA join while offline, then
// bind callbacks and ensure the persistent worker exists. Join failure does not
// prevent worker startup; true reports worker availability, not connectivity.
// Reconnect is checked even when the worker already exists. No radio-mode change
// or credential reload is introduced here; callers retain their resource policy.
bool prepare(const VoiceCallbacks& callbacks, bool reconnectIfOffline = false);
// UI-task configuration operations: update the shared backend preference and
// request socket refresh after the active turn; callers never touch the socket.
// Mode selection is inert when unchanged. A changed selection requests refresh
// even if saving fails, preserving Settings' existing best-effort contract.
void selectBackendMode(bool dev);
// Preserve the URL setter's result; refresh only on success. This overrides the
// current URL and persists the dev URL only in dev mode; saving is not atomic.
bool setBackendUrl(const char* url);
void begin(); // Attach the current PCM storage for this turn.
// UI task, after capture stops. Waits for any ring copy under the existing audio
// lock, then prevents further PCM access until begin(). Does not wait on sockets.
void detachAudio();
void abandon();
bool appendAudio(const int16_t* samples, uint32_t count, void* unused);
void commit();
void cancel(uint8_t tag); // UI task.
// UI task: read-only status. Link and shared turn/ring fields are captured under
// the same lock; ready requires the server's authenticated ready frame.
// done/replyReady belong to the calling UI task.
struct Status {
    bool ready, done, commitPending, commitSent, replyReady, overflowed;
    uint32_t droppedSamples;
};
Status status();
struct ReplyQueueStats { uint32_t bytes, peakBytes, events; };
ReplyQueueStats replyQueueStats();
Result result();
void clearDone();
void clearReplyReady();
const char* outcomeName(Outcome outcome);
} }
