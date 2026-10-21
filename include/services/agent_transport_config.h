#pragma once
#include <stdint.h>
namespace cfg { namespace agent {
/** Persistent WebSocket endpoint appended to the configured backend URL. */
constexpr const char* kWebSocketPath = "/api/device-agent";

/** PCM payload per binary WebSocket frame (512 mono int16 samples). */
constexpr uint32_t kAudioFrameBytes = 1024;
/** Yield to WebSocket receive/control work after at most 128 ms of PCM. */
constexpr uint32_t kAudioFramesPerPump = 4;

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
