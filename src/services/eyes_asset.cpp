#include "services/eyes_asset.h"

#include <Arduino.h>
#include <atomic>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include "config/app_config.h"
#include "core/menu_config.h"
#include "core/board_time.h"
#include "config/ota_trust.h"
#include "core/watchdog.h"
#include "hal/hal_sd.h"
#include "hal/hal_wifi.h"

namespace services {
namespace eyes_asset {
namespace {

constexpr char kTemporaryPath[] = "/eyes/eyes.bmp.tmp";
constexpr size_t kExpectedSize = 438330u;
constexpr uint32_t kRetryIntervalMs = 60000u;
constexpr uint32_t kReadIdleTimeoutMs = 10000u;

// Immutable source for the exact asset shipped in sd_flash/eyes/eyes.bmp.
constexpr char kAssetPath[] = "/assets/f7870535dbe34c0a9c8e7311675c4f5e0ac071df16c87c8e089bedfb4cd49e57/eyes.bmp";
constexpr uint8_t kExpectedSha256[32] = {
    0xF7, 0x87, 0x05, 0x35, 0xDB, 0xE3, 0x4C, 0x0A,
    0x9C, 0x8E, 0x73, 0x11, 0x67, 0x5C, 0x4F, 0x5E,
    0x0A, 0xC0, 0x71, 0xDF, 0x16, 0xC8, 0x7C, 0x8E,
    0x08, 0x9B, 0xED, 0xFB, 0x4C, 0xD4, 0x9E, 0x57,
};

enum class State : uint8_t { NotInspected, Ready, RepairPending };

static State s_state = State::NotInspected;
static std::atomic<bool> s_installedNotification{false};
static std::atomic<bool> s_serviceActive{false};
static uint32_t s_nextAttemptMs = 0;

struct HashContext {
  mbedtls_sha256_context sha;
  bool ok;
};

bool hashChunk(const uint8_t* data, size_t len, void* userData) {
  HashContext* ctx = static_cast<HashContext*>(userData);
  if (!ctx || !ctx->ok) return false;
  ctx->ok = mbedtls_sha256_update_ret(&ctx->sha, data, len) == 0;
  return ctx->ok;
}

bool digestMatches(const uint8_t digest[32]) {
  uint8_t difference = 0;
  for (size_t i = 0; i < sizeof(kExpectedSha256); ++i) {
    difference |= digest[i] ^ kExpectedSha256[i];
  }
  return difference == 0;
}

void removeTemporaryFile() {
  if (hal::sd::exists(kTemporaryPath)) {
    (void)hal::sd::removeFile(kTemporaryPath);
  }
}

bool validateInstalledAsset() {
  HashContext ctx{};
  mbedtls_sha256_init(&ctx.sha);
  ctx.ok = mbedtls_sha256_starts_ret(&ctx.sha, 0) == 0;

  size_t fileSize = 0;
  const bool streamed = ctx.ok &&
      hal::sd::streamFile(HOME_SCREEN_BMP_PATH_PRIMARY, hashChunk, &ctx, &fileSize);
  uint8_t digest[32] = {};
  const bool finished = streamed && ctx.ok &&
      mbedtls_sha256_finish_ret(&ctx.sha, digest) == 0;
  mbedtls_sha256_free(&ctx.sha);
  return finished && fileSize == kExpectedSize && digestMatches(digest);
}

bool downloadAndInstall() {
  if (!OTA_BASE_URL[0] || !core::time::isSynced()) return false;
  char url[320];
  snprintf(url, sizeof(url), "%s%s", OTA_BASE_URL, kAssetPath);

  WiFiClientSecure client;
  client.setCACert(ota_trust::kRootCertificates);
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    Serial.println("[eyes-asset] download setup failed");
    return false;
  }
  http.addHeader("User-Agent", "Origami/eyes-recovery");

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[eyes-asset] download HTTP %d\n", code);
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength >= 0 && (size_t)contentLength != kExpectedSize) {
    Serial.printf("[eyes-asset] unexpected download size %d\n", contentLength);
    http.end();
    return false;
  }

  removeTemporaryFile();
  if (!hal::sd::appendFile(kTemporaryPath, nullptr, 0)) {
    Serial.println("[eyes-asset] could not create temporary file");
    http.end();
    return false;
  }

  HashContext hash{};
  mbedtls_sha256_init(&hash.sha);
  hash.ok = mbedtls_sha256_starts_ret(&hash.sha, 0) == 0;
  WiFiClient* stream = http.getStreamPtr();
  static uint8_t buffer[4096];
  size_t received = 0;
  uint32_t lastDataMs = millis();
  bool ok = hash.ok && stream;

  while (ok && received < kExpectedSize) {
    core::watchdog::feed();
    const int available = stream->available();
    if (available <= 0) {
      if (!stream->connected() ||
          (uint32_t)(millis() - lastDataMs) >= kReadIdleTimeoutMs) {
        ok = false;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    const size_t remaining = kExpectedSize - received;
    const size_t wanted = min((size_t)available, min(sizeof(buffer), remaining));
    const int got = (int)stream->readBytes(buffer, wanted);
    if (got <= 0) continue;
    lastDataMs = millis();
    if (!hal::sd::appendFile(kTemporaryPath, buffer, (size_t)got) ||
        !hashChunk(buffer, (size_t)got, &hash)) {
      ok = false;
      break;
    }
    received += (size_t)got;
  }

  uint8_t digest[32] = {};
  ok = ok && received == kExpectedSize &&
       mbedtls_sha256_finish_ret(&hash.sha, digest) == 0 &&
       digestMatches(digest);
  mbedtls_sha256_free(&hash.sha);
  http.end();

  if (!ok) {
    Serial.printf("[eyes-asset] verification failed after %u/%u bytes\n",
                  (unsigned)received, (unsigned)kExpectedSize);
    removeTemporaryFile();
    return false;
  }
  if (!hal::sd::replaceFile(kTemporaryPath, HOME_SCREEN_BMP_PATH_PRIMARY)) {
    Serial.println("[eyes-asset] install rename failed");
    removeTemporaryFile();
    return false;
  }

  Serial.printf("[eyes-asset] restored %s (%u bytes, SHA-256 verified)\n",
                HOME_SCREEN_BMP_PATH_PRIMARY, (unsigned)received);
  return true;
}

}  // namespace

bool inspect() {
  s_installedNotification = false;
  if (!hal::sd::mounted()) {
    s_state = State::RepairPending;
    Serial.println("[eyes-asset] SD unavailable; procedural eyes remain active");
    return false;
  }

  (void)hal::sd::ensureDir("/eyes");
  removeTemporaryFile();
  if (validateInstalledAsset()) {
    s_state = State::Ready;
    Serial.printf("[eyes-asset] verified %s\n", HOME_SCREEN_BMP_PATH_PRIMARY);
    return true;
  }

  if (hal::sd::exists(HOME_SCREEN_BMP_PATH_PRIMARY)) {
    Serial.printf("[eyes-asset] invalid %s removed; scheduling repair\n",
                  HOME_SCREEN_BMP_PATH_PRIMARY);
    (void)hal::sd::removeFile(HOME_SCREEN_BMP_PATH_PRIMARY);
  } else {
    Serial.printf("[eyes-asset] missing %s; scheduling repair\n",
                  HOME_SCREEN_BMP_PATH_PRIMARY);
  }
  s_state = State::RepairPending;
  s_nextAttemptMs = 0;
  return false;
}

void service() {
  // net_poll and widget_poll may both offer a network opportunity. Exactly one
  // owns the one-shot repair so they cannot open competing TLS/SD sessions.
  if (s_serviceActive.exchange(true, std::memory_order_acquire)) return;
  // State, retry timing and lazy inspection belong to this attempt owner.
  if (s_state == State::NotInspected) (void)inspect();

  const uint32_t now = millis();
  if (s_state == State::RepairPending && hal::sd::mounted() &&
      hal::wifi::connected() &&
      (s_nextAttemptMs == 0 || (int32_t)(now - s_nextAttemptMs) >= 0)) {
    s_nextAttemptMs = now + kRetryIntervalMs;
    Serial.println("[eyes-asset] downloading canonical eyes bitmap");
    if (downloadAndInstall()) {
      s_state = State::Ready;
      s_installedNotification = true;
    }
  }
  s_serviceActive.store(false, std::memory_order_release);
}

bool takeInstalledNotification() {
  return s_installedNotification.exchange(false);
}

}  // namespace eyes_asset
}  // namespace services
