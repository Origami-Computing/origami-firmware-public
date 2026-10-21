#pragma once
#include <stdint.h>
#include <ArduinoJson.h>

// One bounded background text request over the existing Device Agent socket.
// No feature-specific prompt, cache, or UI state belongs in this transport.
namespace core { namespace device_agent_text {
enum class Outcome : uint8_t { Ready, Error, Unsupported };
struct Result {
  Outcome outcome = Outcome::Error;
  bool retryable = false;
  char text[512] = {};
  char error[80] = {};
};
bool submit(const char* requestId, const char* text);
// Thread-safe admission query: true while an unfinished text turn needs the
// socket, including queued/reconnecting turns. Does not reserve the transport.
bool hasPendingRequest();
bool take(Result& out);
void cancel();
// Called only by the existing WebSocket owner task.
void disconnected();
void ready(bool supportsText);
bool receive(const JsonDocument& event);
void pump(uint32_t now, bool connected, bool voiceActive, bool (*send)(const char*));
} }
