#pragma once

#include <stddef.h>
#include <stdint.h>

namespace services {
namespace remote_display {

static constexpr uint16_t kDefaultPort = 4210;
static constexpr uint16_t kPanelW = 280;
static constexpr uint16_t kPanelH = 240;
static constexpr size_t kMaxPayloadBytes = 128u * 1024u;

struct Frame {
  const uint8_t* data = nullptr;
  size_t len = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t frameId = 0;
};

struct Stats {
  bool listening = false;
  bool connected = false;
  bool frameReady = false;
  uint16_t port = kDefaultPort;
  uint16_t sourceW = 0;
  uint16_t sourceH = 0;
  uint32_t frameId = 0;
  uint32_t framesReceived = 0;
  uint32_t framesDropped = 0;
  uint32_t badHeaders = 0;
  uint32_t bytesReceived = 0;
  uint32_t lastFrameMs = 0;
  uint32_t lastClientMs = 0;
};

// UI-task session: start requests Wi-Fi restoration; service retries listener
// allocation while active. stop closes the client/listener and releases payload.
bool start(uint16_t port = kDefaultPort);
void stop();
void service(uint32_t now);

bool frameReady();
// Borrowed until consumeFrame() or stop(); render synchronously on the UI task.
Frame frame();
void consumeFrame();

Stats stats();
const char* localIpString();

}  // namespace remote_display
}  // namespace services
