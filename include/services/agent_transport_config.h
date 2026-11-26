#pragma once
#include <stdint.h>
namespace cfg { namespace agent {
/** Persistent WebSocket endpoint appended to the configured backend URL. */
constexpr const char* kWebSocketPath = "/api/device-agent";

/** Maximum PCM payload per frame. Send available samples immediately; never
 * wait to fill this. Larger frames let a reconnect backlog drain efficiently. */
constexpr uint32_t kAudioFrameBytes = 4096;
/** Yield to receive/cancel work after at most 256 ms of queued PCM. */
constexpr uint32_t kAudioFramesPerPump = 2;

/** Reconnect if authenticated ready or the turn start acknowledgment never arrives. */
constexpr uint32_t kReadyTimeoutMs = 8000;

/** Maximum time from commit until a completed agent response. */
constexpr uint32_t kResponseTimeoutMs = 90000;

/** Grace period from release for WiFi/TLS/WebSocket to connect and send commit.
 *  Recording has already been buffered locally, so this can be longer than the
 *  UI warmup hint without losing audio. */
constexpr uint32_t kConnectGraceMs = 45000;

/** How long the socket must stay connected before streaming audio. Kept short so
 *  the live ring drains quickly; reconnect recovery handles post-camera drops. */
constexpr uint32_t kWsStableMs = 500;

/** Server-side session identifier. */
constexpr const char* kSessionId = "origami-device";

/** Server-side platform identifier. */
constexpr const char* kPlatform = "device";

} }
