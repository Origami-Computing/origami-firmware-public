#include "services/remote_display_stream.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "hal/hal_wifi.h"

namespace services {
namespace remote_display {
namespace {

static constexpr uint8_t kVersion = 1;
static constexpr uint8_t kKindJpegFrame = 1;
static constexpr size_t kHeaderLen = 20;
static constexpr uint32_t kClientIdleMs = 5000;

bool s_requested = false;
WiFiServer* s_server = nullptr;
WiFiClient s_client;
uint16_t s_port = kDefaultPort;
uint8_t* s_payload = nullptr;
uint32_t s_payloadCap = 0;

uint8_t s_header[kHeaderLen] = {};
size_t s_headerGot = 0;
size_t s_payloadGot = 0;
uint32_t s_expectedPayload = 0;
uint16_t s_frameW = 0;
uint16_t s_frameH = 0;
uint32_t s_frameId = 0;

bool s_frameReady = false;
size_t s_readyLen = 0;
uint16_t s_readyW = 0;
uint16_t s_readyH = 0;
uint32_t s_readyId = 0;

Stats s_stats;
char s_ip[24] = "0.0.0.0";

enum class RxState : uint8_t { Header, Payload };
RxState s_rxState = RxState::Header;

static uint16_t le16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void resetRx() {
  s_headerGot = 0;
  s_payloadGot = 0;
  s_expectedPayload = 0;
  s_frameW = 0;
  s_frameH = 0;
  s_frameId = 0;
  s_rxState = RxState::Header;
}

static void dropClient() {
  if (s_client) s_client.stop();
  resetRx();
}

static bool ensurePayload() {
  if (s_payload) return true;
  s_payload = (uint8_t*)heap_caps_malloc(kMaxPayloadBytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_payload) {
    s_payload = (uint8_t*)heap_caps_malloc(kMaxPayloadBytes, MALLOC_CAP_8BIT);
  }
  if (!s_payload) {
    Serial.println("[remote-display] payload buffer alloc failed");
    return false;
  }
  s_payloadCap = kMaxPayloadBytes;
  Serial.printf("[remote-display] payload buffer ready: %u bytes\n",
                (unsigned)s_payloadCap);
  return true;
}

static void publishIp() {
  if (hal::wifi::connected()) {
    IPAddress ip(WiFi.localIP());
    snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  } else {
    snprintf(s_ip, sizeof(s_ip), "WiFi offline");
  }
}

static bool parseHeader() {
  if (memcmp(s_header, "KAMI", 4) != 0 ||
      s_header[4] != kVersion ||
      s_header[5] != kKindJpegFrame) {
    s_stats.badHeaders++;
    return false;
  }

  const uint16_t headerLen = le16(s_header + 6);
  s_frameW = le16(s_header + 8);
  s_frameH = le16(s_header + 10);
  s_frameId = le32(s_header + 12);
  s_expectedPayload = le32(s_header + 16);

  if (headerLen != kHeaderLen ||
      s_frameW == 0 || s_frameH == 0 ||
      s_frameW > kPanelW || s_frameH > kPanelH ||
      s_expectedPayload == 0 ||
      s_expectedPayload > s_payloadCap) {
    s_stats.badHeaders++;
    return false;
  }
  return true;
}

static void readHeader() {
  while (s_client.connected() && s_client.available() > 0 && s_headerGot < kHeaderLen) {
    int c = s_client.read();
    if (c < 0) break;
    s_header[s_headerGot++] = (uint8_t)c;
  }
  if (s_headerGot < kHeaderLen) return;

  if (!parseHeader()) {
    Serial.println("[remote-display] bad frame header; dropping client");
    dropClient();
    return;
  }
  s_payloadGot = 0;
  s_rxState = RxState::Payload;
}

static void readPayload(uint32_t now) {
  if (s_expectedPayload == 0) {
    resetRx();
    return;
  }

  while (s_client.connected() && s_client.available() > 0 && s_payloadGot < s_expectedPayload) {
    const size_t remain = s_expectedPayload - s_payloadGot;
    const size_t chunk = remain > 1024 ? 1024 : remain;
    int n = s_client.read(s_payload + s_payloadGot, chunk);
    if (n <= 0) break;
    s_payloadGot += (size_t)n;
    s_stats.bytesReceived += (uint32_t)n;
    s_stats.lastClientMs = now;
  }

  if (s_payloadGot < s_expectedPayload) return;

  if (s_frameReady) {
    s_stats.framesDropped++;
  }
  s_readyLen = s_expectedPayload;
  s_readyW = s_frameW;
  s_readyH = s_frameH;
  s_readyId = s_frameId;
  s_frameReady = true;

  s_stats.framesReceived++;
  s_stats.frameId = s_frameId;
  s_stats.sourceW = s_frameW;
  s_stats.sourceH = s_frameH;
  s_stats.lastFrameMs = now;
  s_stats.lastClientMs = now;
  resetRx();
}

static bool ensureListening() {
  publishIp();
  if (!ensurePayload()) return false;
  if (s_server) {
    s_stats.listening = true;
    return true;
  }
  s_server = new WiFiServer(s_port);
  if (!s_server) return false;
  s_server->begin();
  s_server->setNoDelay(true);
  s_stats.listening = true;
  Serial.printf("[remote-display] listening on %s:%u\n", s_ip, (unsigned)s_port);
  return true;
}

}  // namespace

bool start(uint16_t port) {
  s_requested = true;
  s_port = port ? port : kDefaultPort;
  s_stats.port = s_port;
  hal::wifi::scheduleRestore();
  return ensureListening();
}

void stop() {
  s_requested = false;
  if (s_client) s_client.stop();
  if (s_server) {
    s_server->stop();
    delete s_server;
    s_server = nullptr;
  }
  if (s_payload) {
    heap_caps_free(s_payload);
    s_payload = nullptr;
    s_payloadCap = 0;
  }
  resetRx();
  s_frameReady = false;
  s_readyLen = 0;
  s_stats.connected = false;
  s_stats.listening = false;
  Serial.println("[remote-display] stopped");
}

void service(uint32_t now) {
  if (!s_requested) return;
  ensureListening();
  s_stats.listening = s_server != nullptr;
  s_stats.connected = s_client && s_client.connected();
  s_stats.frameReady = s_frameReady;

  if (!s_server || !hal::wifi::connected()) return;

  if (!s_client || !s_client.connected()) {
    WiFiClient next = s_server->available();
    if (next) {
      if (s_client) s_client.stop();
      s_client = next;
      s_client.setNoDelay(true);
      resetRx();
      s_stats.connected = true;
      s_stats.lastClientMs = now;
      Serial.println("[remote-display] host connected");
    }
    return;
  }

  if ((uint32_t)(now - s_stats.lastClientMs) > kClientIdleMs) {
    Serial.println("[remote-display] client idle timeout");
    dropClient();
    s_stats.connected = false;
    return;
  }

  if (s_frameReady) return;

  if (s_rxState == RxState::Header) readHeader();
  if (s_rxState == RxState::Payload) readPayload(now);
  s_stats.connected = s_client && s_client.connected();
  s_stats.frameReady = s_frameReady;
}

bool frameReady() {
  return s_frameReady;
}

Frame frame() {
  Frame f;
  if (!s_frameReady) return f;
  f.data = s_payload;
  f.len = s_readyLen;
  f.width = s_readyW;
  f.height = s_readyH;
  f.frameId = s_readyId;
  return f;
}

void consumeFrame() {
  s_frameReady = false;
  s_readyLen = 0;
  s_stats.frameReady = false;
}

Stats stats() {
  s_stats.connected = s_client && s_client.connected();
  s_stats.listening = s_server != nullptr;
  s_stats.frameReady = s_frameReady;
  return s_stats;
}

const char* localIpString() {
  publishIp();
  return s_ip;
}

}  // namespace remote_display
}  // namespace services
