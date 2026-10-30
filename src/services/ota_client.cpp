#ifndef OTA_BUILD_CHANNEL
#define OTA_BUILD_CHANNEL "public"
#endif
#include "services/ota_client.h"
#include "services/ota_access.h"
#include "services/manufacturing.h"
#include "services/firmware_update.h"
#include "core/menu_config.h"
#include "core/release_policy.h"
#include "core/store.h"
#include "core/board_time.h"
#include "hal/hal_tasks.h"
#include <esp_heap_caps.h>
#include "config/ota_trust.h"

#include <atomic>
#include <memory>
#include <ctype.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// Arduino otherwise accepts an OTA image before setup() and the UI health gate.
// Leave the bootloader pending state intact for serviceBootHealth().
extern "C" bool verifyRollbackLater() { return true; }

namespace services {
namespace ota_client {

static char s_err[96] = {};
static constexpr const char* kSettingsNamespace = "settings";
static constexpr const char* kChannelKey = "ota_chan";
static bool s_initialized = false;
static bool s_bootPendingVerify = false;
static bool s_bootHealthHandled = false;
static uint32_t s_bootHealthStartMs = 0;

enum class DownloadState { Idle, Running, Ready, Failed };
static std::atomic<DownloadState> s_download{DownloadState::Idle};
static std::atomic<bool> s_cancel{false};
static std::atomic<int> s_written{0};
static ReleaseInfo s_release{};
static char s_downloadKey[ota_access::kKeySize] = {};
static void setError(const char* msg);
static bool refreshAccess();
static const esp_partition_t* s_candidate = nullptr;

const char* lastError() { return s_err; }

void init() {
    if (s_initialized) return;
    s_initialized = true;
    // Retain legacy NVS credential until this bridge firmware has passed boot
    // health. If it rolls back, the old firmware still needs GitHub access.
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    s_bootPendingVerify = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                          state == ESP_OTA_IMG_PENDING_VERIFY;
    s_bootHealthHandled = !s_bootPendingVerify;
    s_bootHealthStartMs = millis();
    Serial.printf("[ota] transport=cdn build=%s hw=%s layout=%s confirmation=required pending_verify=%u\n",
                  buildKindName(), OTA_HARDWARE_ID, OTA_LAYOUT_ID,
                  s_bootPendingVerify ? 1u : 0u);
}

bool isOfficialBuild() { return OTA_BUILD_OFFICIAL != 0; }

const char* buildKindName() { return isOfficialBuild() ? "official" : "custom"; }

void serviceBootHealth(bool setupHealthy) {
    if (s_bootHealthHandled || !s_bootPendingVerify) return;
    if ((uint32_t)(millis() - s_bootHealthStartMs) < 30000u) return;

    if (!setupHealthy) {
        Serial.println("[ota] boot health failed; rolling back to previous image");
        s_bootHealthHandled = true;
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        Serial.println("[ota] boot health confirmed; running image marked valid");
        s_bootHealthHandled = true;
        s_bootPendingVerify = false;
        // Remove only the obsolete GitHub credential, never the new OTA key.
        Preferences legacy;
        if (legacy.begin("ota_auth", false)) { legacy.remove("github_token"); legacy.end(); }
    } else {
        Serial.printf("[ota] failed to mark running image valid: %d\n", (int)err);
    }
}

bool hasAuthToken() { char key[ota_access::kKeySize]; return ota_access::read(key); }
bool setAuthToken(const char* token) {
    if(firmware_update::noticeBusy()){setError("OTA check busy; retry");return false;}
    abortFlash();
    // Changing enrollment invalidates the download/role evidence for shipping.
    uint32_t passed=0,failed=0;
    if(manufacturing::report(passed,failed)&&!manufacturing::invalidate(13))return false;
    // Reset selection before replacement: a failed NVS write must not retain
    // staging selection under a newly provisioned production key.
    if (!core::store::putU8(kSettingsNamespace, kChannelKey, 0)) return false;
    if (!ota_access::store(token)) { setError("OTA key save failed"); return false; }
    firmware_update::invalidateSettingsNotice();
    return true;
}

// Apply the Public default once per device. Later explicit developer choices
// survive reboots. A partial NVS write leaves selection on Public and retries
// after reboot; never repeatedly write from Settings rendering.
static bool s_publicDefaultAttempted = false;
static bool s_publicDefaultReady = false;
static bool ensurePublicDefault() {
    if (s_publicDefaultAttempted) return s_publicDefaultReady;
    s_publicDefaultAttempted = true;
    s_publicDefaultReady = core::store::getU8(kSettingsNamespace, "ota_pub_v1", 0) == 1;
    if (!s_publicDefaultReady) {
        s_publicDefaultReady = core::store::putU8(kSettingsNamespace, kChannelKey, 0)
            && core::store::putU8(kSettingsNamespace, "ota_pub_v1", 1);
    }
    return s_publicDefaultReady;
}

ReleaseChannel releaseChannel() {
    if (!ensurePublicDefault() || !hasAuthToken()) return ReleaseChannel::Public;
    const uint8_t saved = core::store::getU8(kSettingsNamespace, kChannelKey, 0);
    return saved == 1 ? ReleaseChannel::Staging :
           saved == 2 ? ReleaseChannel::Private : ReleaseChannel::Public;
}

bool setReleaseChannel(ReleaseChannel channel) {
    s_err[0] = '\0';
    if (channel != ReleaseChannel::Public && channel != ReleaseChannel::Staging && channel != ReleaseChannel::Private) {
        setError("Invalid update channel"); return false;
    }
    if (!ensurePublicDefault()) { setError("Public default save failed; restart to retry"); return false; }
    if (channel != ReleaseChannel::Public) {
        // Missing enrollment is actionable even if another network owner is busy.
        if (!hasAuthToken()) { setError("Set up OTA access key"); return false; }
        firmware_update::Session session;
        if (session.reserveNetwork() != firmware_update::Session::Result::Ready) {
            setError("OTA network busy"); return false;
        }
        if (!refreshAccess()) return false;
        if (ota_access::role() != ota_access::Role::Developer) {
            setError("Developer OTA access required"); return false;
        }
    }
    if (!core::store::putU8(kSettingsNamespace, kChannelKey,
                          static_cast<uint8_t>(channel))) {
        setError("Update channel save failed"); return false;
    }
    return true;
}

const char* releaseChannelName(ReleaseChannel channel) {
    return channel == ReleaseChannel::Staging ? "Staging" :
           channel == ReleaseChannel::Private ? "Private" : "Public";
}

struct ParsedVersion {
    int major;
    int minor;
    int patch;
    int stagingRun;
};

static bool parseVersion(const char* tag, ParsedVersion* out) {
    if (!tag || !out) return false;
    int consumed = 0;
    ParsedVersion parsed = {0, 0, 0, -1};
    if (sscanf(tag, "v%d.%d.%d%n", &parsed.major, &parsed.minor,
               &parsed.patch, &consumed) != 3) return false;
    const char* suffix = tag + consumed;
    if (*suffix) {
        int suffixConsumed = 0;
        if (sscanf(suffix, "-staging.%d%n", &parsed.stagingRun,
                   &suffixConsumed) != 1 || suffix[suffixConsumed] != '\0') {
            return false;
        }
    }
    *out = parsed;
    return true;
}

bool isReleaseNewer(ReleaseChannel channel, const char* candidateTag) {
    ParsedVersion candidate;
    ParsedVersion current;
    if (!parseVersion(candidateTag, &candidate) ||
        !parseVersion(OTA_FIRMWARE_VERSION, &current)) return false;
    if (channel != ReleaseChannel::Staging && candidate.stagingRun >= 0) return false;

    // Channel choice is explicit and role-checked by the production owner.
    // Independent channels need not share version numbers (public 1.0 may
    // coexist with private 0.x). Never treat that choice as a same-channel
    // automatic rollback. Signature, board/layout and confirmation still apply.
    const char* selected = channel == ReleaseChannel::Staging ? "staging" :
                           channel == ReleaseChannel::Private ? "private" : "public";
    if (strcmp(OTA_BUILD_CHANNEL, selected) != 0) return true;
    if (candidate.major != current.major) return candidate.major > current.major;
    if (candidate.minor != current.minor) return candidate.minor > current.minor;
    if (candidate.patch != current.patch) return candidate.patch > current.patch;
    // A developer may need to replace a custom build with the official build
    // carrying the same human-readable version.
    if (!isOfficialBuild()) return true;
    if (channel == ReleaseChannel::Staging) {
        return candidate.stagingRun > current.stagingRun;
    }
    // A stable release of the same base version supersedes a staging build.
    return candidate.stagingRun < 0 && current.stagingRun >= 0;
}

static void setError(const char* msg) {
    Serial.printf("[ota] error=%s\n", msg ? msg : "unknown");
    strlcpy(s_err, msg ? msg : "unknown", sizeof(s_err));
}

static const char* channelPath(ReleaseChannel channel) {
    return channel == ReleaseChannel::Staging ? "staging" :
           channel == ReleaseChannel::Private ? "private" : "public";
}

static void configureHttp(HTTPClient& http, WiFiClientSecure& client) {
    client.setCACert(ota_trust::kRootCertificates);
    client.setHandshakeTimeout(12);
    http.setConnectTimeout(12000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.useHTTP10(true); // S3/CloudFront returns a bounded Content-Length body.
}

// Called only on the UI/service thread; the worker receives a key snapshot.
static bool refreshAccess() {
    ota_access::confirm(nullptr, ota_access::Role::Unknown);
    char key[ota_access::kKeySize];
    if (!ota_access::read(key)) { setError("Set up OTA access key"); return false; }
    core::time::service(millis());
    if (!core::time::isSynced()) { setError("waiting for network time"); return false; }
    WiFiClientSecure client;
    HTTPClient http;
    configureHttp(http, client);
    if (!http.begin(client, OTA_BASE_URL "/access")) { setError("access connect failed"); return false; }
    http.addHeader("Authorization", String("Bearer ") + key);
    const int code = http.GET();
    // Only two small exact responses are accepted. No unbounded String reads.
    char body[11] = {};
    const int length = http.getSize();
    WiFiClient* stream = code == 200 && length > 0 && length < (int)sizeof(body)
        ? http.getStreamPtr() : nullptr;
    const size_t got = stream ? stream->readBytes(body, length) : 0;
    http.end();
    const auto role = got == 9 && !memcmp(body, "developer", 9) ? ota_access::Role::Developer
                    : got == 10 && !memcmp(body, "production", 10) ? ota_access::Role::Production
                    : ota_access::Role::Unknown;
    if (role == ota_access::Role::Unknown) {
        char error[64];
        if (code == 200) setError("Invalid OTA access response");
        else {
            snprintf(error,sizeof(error),"OTA access HTTP %d; check provisioning",code);
            setError(error);
        }
        return false;
    }
    ota_access::confirm(key, role);
    if (role == ota_access::Role::Production &&
        core::store::getU8(kSettingsNamespace, kChannelKey, 0) != 0 &&
        !core::store::putU8(kSettingsNamespace, kChannelKey, 0)) {
        setError("channel save failed"); return false;
    }
    return true;
}

static bool decodeHash(const char* text, uint8_t out[32]) {
    if (!text || strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; ++j) {
            const char c = text[i * 2 + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            value = value * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = value;
    }
    return true;
}

bool fetchLatestRelease(ReleaseChannel channel, ReleaseInfo* out) {
    if (!out || s_download.load() == DownloadState::Running) return false;
    if (channel != ReleaseChannel::Public && channel != ReleaseChannel::Staging && channel != ReleaseChannel::Private) return false;
    s_err[0] = '\0';
    if (!refreshAccess()) return false;
    if (channel != ReleaseChannel::Public && ota_access::role() != ota_access::Role::Developer) {
        setError("Developer OTA access required"); return false;
    }
    if (!OTA_BASE_URL[0]) { setError("update host not configured"); return false; }
    // Never disable certificate validation to work around an unset clock.
    core::time::service(millis());
    if (!core::time::isSynced()) { setError("waiting for network time"); return false; }
    char url[384];
    const int urlLen = snprintf(url, sizeof(url), "%s/channels/%s/%s/%s.json",
                               OTA_BASE_URL, channelPath(channel), OTA_HARDWARE_ID, OTA_LAYOUT_ID);
    if (urlLen < 0 || urlLen >= (int)sizeof(url)) { setError("manifest URL too long"); return false; }
    WiFiClientSecure client;
    HTTPClient http;
    configureHttp(http, client);
    if (!http.begin(client, url)) { setError("manifest connect failed"); return false; }
    char accessKey[ota_access::kKeySize];
    if (!ota_access::read(accessKey)) { http.end(); setError("Set up OTA access key"); return false; }
    http.addHeader("Authorization", String("Bearer ") + accessKey);
    const int code = http.GET();
    const int length = http.getSize();
    if (code != HTTP_CODE_OK || length <= 0 || length > 4096) {
        char error[64];
        snprintf(error, sizeof(error), "manifest HTTP %d / invalid size", code);
        setError(error); http.end(); return false;
    }
    // Read exactly the bounded document; reject truncated/chunked responses.
    std::unique_ptr<char[]> body(new (std::nothrow) char[length + 1]);
    if (!body) { setError("manifest memory"); http.end(); return false; }
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) { setError("manifest disconnected"); http.end(); return false; }
    stream->setTimeout(15000);
    const size_t got = stream->readBytes(body.get(), length);
    http.end();
    if (got != (size_t)length) { setError("manifest incomplete"); return false; }
    body[length] = '\0';
    JsonDocument envelope;
    if (deserializeJson(envelope, body.get(), length)) { setError("manifest JSON"); return false; }
    const char* payload = envelope["payload"] | "";
    const char* signature = envelope["signature"] | "";
    if (!payload[0] || strlen(payload) > 2048 || strlen(signature) != 344) {
        setError("invalid signed manifest"); return false;
    }
    uint8_t sig[256], digest[32];
    size_t sigLength = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &sigLength,
            (const uint8_t*)signature, strlen(signature)) || sigLength != sizeof(sig) ||
        mbedtls_sha256_ret((const uint8_t*)payload, strlen(payload), digest, 0)) {
        setError("manifest signature encoding"); return false;
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    const bool signatureOk = mbedtls_pk_parse_public_key(&key,
        (const uint8_t*)ota_trust::kSigningPublicKey, sizeof(ota_trust::kSigningPublicKey)) == 0 &&
        mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), sig, sizeof(sig)) == 0;
    mbedtls_pk_free(&key);
    if (!signatureOk) { setError("manifest signature invalid"); return false; }
    JsonDocument manifest;
    if (deserializeJson(manifest, payload)) { setError("signed payload JSON"); return false; }
    const char* version = manifest["version"] | "";
    const char* path = manifest["path"] | "";
    ParsedVersion parsed{};
    char prefix[160];
    snprintf(prefix, sizeof(prefix), "/releases/%s/%s/%s/", channelPath(channel),
             OTA_HARDWARE_ID, OTA_LAYOUT_ID);
    ReleaseInfo release{};
    if (manifest["schema"].as<int>() != 1 ||
        strcmp(manifest["channel"] | "", channelPath(channel)) ||
        strcmp(manifest["hardware"] | "", OTA_HARDWARE_ID) ||
        strcmp(manifest["layout"] | "", OTA_LAYOUT_ID) ||
        !parseVersion(version, &parsed) || strlen(version) >= sizeof(release.tag) ||
        (channel == ReleaseChannel::Staging) != (parsed.stagingRun >= 0) ||
        strncmp(path, prefix, strlen(prefix)) || strstr(path, "..") ||
        !manifest["size"].is<int>() || !decodeHash(manifest["sha256"] | "", release.sha256)) {
        setError("manifest incompatible"); return false;
    }
    for (const char* p = path; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '/' || *p == '-' || *p == '.')) {
            setError("invalid image path"); return false;
        }
    }
    release.size = manifest["size"].as<int>();
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part || release.size <= 0 || (size_t)release.size > part->size) {
        setError("image does not fit"); return false;
    }
    release.channel = channel;
    strlcpy(release.tag, version, sizeof(release.tag));
    const int imageUrlLen = snprintf(release.url, sizeof(release.url), "%s%s", OTA_BASE_URL, path);
    if (imageUrlLen < 0 || imageUrlLen >= (int)sizeof(release.url)) {
        setError("image URL too long"); return false;
    }
    *out = release;
    return true;
}

// The worker exclusively owns its TLS connection, hash and OTA handle. The UI
// only reads atomics; terminal state is published after all resources are closed.
enum class AttemptResult { Success, Retry, Failed };
// Reopen only the signed immutable image URL. A resumed response must describe
// exactly the unwritten suffix; the original full-image hash remains authoritative.
static AttemptResult resumeDownload(HTTPClient& http, WiFiClientSecure& client, int written) {
    http.end();
    configureHttp(http, client);
    if (!http.begin(client, s_release.url)) { setError("resume connect failed"); return AttemptResult::Retry; }
    const char* headers[] = {"Content-Range"};
    http.collectHeaders(headers, 1);
    http.addHeader("Authorization", String("Bearer ") + s_downloadKey);
    char range[48];
    snprintf(range, sizeof(range), "bytes=%d-", written);
    http.addHeader("Range", range);
    const int code = http.GET();
    if (code != HTTP_CODE_PARTIAL_CONTENT) {
        setError("resume HTTP rejected");
        return code < 0 || code == 429 || code >= 500 ? AttemptResult::Retry : AttemptResult::Failed;
    }
    char expected[96];
    snprintf(expected, sizeof(expected), "bytes %d-%d/%d", written, s_release.size - 1, s_release.size);
    if (http.getSize() != s_release.size - written || http.header("Content-Range") != expected) {
        setError("resume range mismatch"); return AttemptResult::Failed;
    }
    if (!http.getStreamPtr()) { setError("resume disconnected"); return AttemptResult::Retry; }
    return AttemptResult::Success;
}

static AttemptResult downloadAttempt() {
    constexpr size_t kBufferSize = 8192;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> buffer(
        static_cast<uint8_t*>(heap_caps_malloc(kBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!buffer) { setError("download buffer memory"); return AttemptResult::Failed; }
    WiFiClientSecure client;
    HTTPClient http;
    configureHttp(http, client);
    if (!http.begin(client, s_release.url)) { setError("download connect failed"); return AttemptResult::Retry; }
    http.addHeader("Authorization", String("Bearer ") + s_downloadKey);
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char error[48]; snprintf(error, sizeof(error), "download HTTP %d", code); setError(error);
        http.end();
        return code < 0 || code == 429 || code >= 500 ? AttemptResult::Retry : AttemptResult::Failed;
    }
    if (http.getSize() != s_release.size) {
        setError("download size mismatch"); http.end(); return AttemptResult::Failed;
    }
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) { setError("download interrupted"); http.end(); return AttemptResult::Retry; }
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(s_candidate, s_release.size, &handle) != ESP_OK) {
        setError("ota_begin failed"); http.end(); return AttemptResult::Failed;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool hashOk = mbedtls_sha256_starts_ret(&sha, 0) == 0;
    uint32_t lastData = millis();
    const uint32_t start = lastData;
    int written = 0;
    unsigned resumes = 0;
    s_written.store(0);
    AttemptResult result = AttemptResult::Retry;
    while (!s_cancel.load() && hashOk && written < s_release.size) {
        if ((uint32_t)(millis() - start) > 600000u) { setError("download deadline"); break; }
        int available = stream->available();
        if (available <= 0) {
            if (!stream->connected() || (uint32_t)(millis() - lastData) > 15000u) {
                if (written > 0 && resumes < 2 && !s_cancel.load()) {
                    ++resumes;
                    result = resumeDownload(http, client, written);
                    if (result != AttemptResult::Success) break;
                    stream = http.getStreamPtr();
                    if (!stream) { setError("resume disconnected"); result = AttemptResult::Retry; break; }
                    Serial.printf("[ota] resume offset=%d attempt=%u\n", written, resumes);
                    lastData = millis();
                    result = AttemptResult::Retry;
                    continue;
                }
                setError("download interrupted"); break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const int wanted = min(available, min((int)kBufferSize, s_release.size - written));
        const int count = stream->read(buffer.get(), wanted);
        if (count <= 0) {
            if ((uint32_t)(millis() - lastData) > 15000u) {
                // Force the next poll through the disconnected/resume path.
                client.stop();
            }
            vTaskDelay(1); continue;
        }
        lastData = millis();
        if (esp_ota_write(handle, buffer.get(), count) != ESP_OK) {
            setError("flash write failed"); result = AttemptResult::Failed; break;
        }
        hashOk = mbedtls_sha256_update_ret(&sha, buffer.get(), count) == 0;
        written += count;
        s_written.store(written);
        vTaskDelay(1); // Give WiFi and the idle watchdog task time to run.
    }
    uint8_t digest[32];
    if (s_cancel.load()) { setError("download cancelled"); result = AttemptResult::Failed; }
    else if (!hashOk) { setError("hash failed"); result = AttemptResult::Failed; }
    else if (written == s_release.size) {
        if (mbedtls_sha256_finish_ret(&sha, digest) || memcmp(digest, s_release.sha256, 32)) {
            setError("firmware hash mismatch"); result = AttemptResult::Failed;
        } else {
            const esp_err_t end = esp_ota_end(handle);
            handle = 0; // esp_ota_end frees the handle, including on validation failure.
            if (end == ESP_OK) result = AttemptResult::Success;
            else { setError("image validation failed"); result = AttemptResult::Failed; }
        }
    }
    if (handle) esp_ota_abort(handle);
    mbedtls_sha256_free(&sha);
    http.end();
    return result;
}

static void downloadTask(void*) {
    AttemptResult result = AttemptResult::Failed;
    const uint32_t start = millis();
    for (unsigned attempt = 0; attempt < 3 && !s_cancel.load(); ++attempt) {
        if (attempt) {
            const uint32_t waitMs = (1000u << attempt) + esp_random() % 1000;
            const uint32_t pauseStart = millis();
            while (!s_cancel.load() && (uint32_t)(millis() - pauseStart) < waitMs) vTaskDelay(pdMS_TO_TICKS(20));
            if (s_cancel.load()) break;
        }
        result = downloadAttempt();
        if (result != AttemptResult::Retry) break;
    }
    const bool ok = result == AttemptResult::Success && !s_cancel.load();
    if (ok) s_err[0] = '\0';
    Serial.printf("[ota] download=%s bytes=%d elapsed_ms=%lu\n", ok ? "verified" : "failed",
                  s_written.load(), (unsigned long)(millis() - start));
    Serial.printf("[ota] worker minFreeStack=%u\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    s_download.store(ok ? DownloadState::Ready : DownloadState::Failed);
    hal::tasks::recordCurrentStack(hal::tasks::Id::OtaDownload);
    hal::tasks::deleteCurrent(hal::tasks::Id::OtaDownload);
}

bool beginFlash(const ReleaseInfo& release) {
    abortFlash();
    s_err[0] = '\0';
    if (!refreshAccess()) return false;
    if (release.channel != ReleaseChannel::Public && release.channel != ReleaseChannel::Staging && release.channel != ReleaseChannel::Private) return false;
    if (release.channel != ReleaseChannel::Public && ota_access::role() != ota_access::Role::Developer) {
        setError("Developer OTA access required"); return false;
    }
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s/releases/%s/%s/%s/", OTA_BASE_URL,
             channelPath(release.channel), OTA_HARDWARE_ID, OTA_LAYOUT_ID);
    if (strncmp(release.url, prefix, strlen(prefix)) || strstr(release.url, "..")) {
        setError("invalid download origin/path"); return false;
    }
    if (!ota_access::read(s_downloadKey)) { setError("Set up OTA access key"); return false; }
    s_candidate = esp_ota_get_next_update_partition(nullptr);
    if (!s_candidate || release.size <= 0 || (size_t)release.size > s_candidate->size) {
        setError("image does not fit"); return false;
    }
    s_release = release;
    s_written.store(0);
    s_cancel.store(false);
    s_download.store(DownloadState::Running);
    if (!hal::tasks::spawn(hal::tasks::Id::OtaDownload, downloadTask)) {
        s_download.store(DownloadState::Failed);
        setError("download task memory"); return false;
    }
    return true;
}

bool pumpFlash(int* written_out, int* total_out) {
    const DownloadState state = s_download.load();
    if (written_out) *written_out = s_written.load();
    if (total_out) *total_out = s_release.size;
    return state == DownloadState::Running;
}

bool finishFlash() {
    if (s_download.load() != DownloadState::Ready || !s_candidate) {
        if (!s_err[0]) setError("download not verified");
        return false;
    }
    if (esp_ota_set_boot_partition(s_candidate) != ESP_OK) { setError("set_boot failed"); return false; }
    s_download.store(DownloadState::Idle);
    s_candidate = nullptr;
    memset(s_downloadKey, 0, sizeof(s_downloadKey));
    return true;
}

void abortFlash() {
    s_cancel.store(true);
    // Do not kill a task while it owns an OTA handle or TLS resources.
    while (s_download.load() == DownloadState::Running) vTaskDelay(pdMS_TO_TICKS(10));
    s_download.store(DownloadState::Idle);
    s_candidate = nullptr;
    memset(s_downloadKey, 0, sizeof(s_downloadKey));
}

}  // namespace ota_client
}  // namespace services
