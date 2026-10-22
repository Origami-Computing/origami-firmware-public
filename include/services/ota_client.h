#pragma once

#include <stdint.h>

namespace services {
namespace ota_client {

enum class ReleaseChannel : uint8_t {
    Public     = 0,
    Production = Public, // Source compatibility; never maps to private firmware.
    Staging    = 1, // Preserve existing saved staging selection.
    Private    = 2,
};

struct ReleaseInfo {
    ReleaseChannel channel = ReleaseChannel::Production;
    char tag[32];
    char url[512];  // Verified HTTPS origin + signed immutable image path
    int  size;
    uint8_t sha256[32]; // Image digest authenticated by the manifest signature
};

// Initialise build identity and boot-health tracking. Access key is provisioned in NVS.
void init();
// Provision/clear a 64-hex OTA access key. Cancels pending OTA and resets channel.
bool hasAuthToken();
bool setAuthToken(const char* token);

// Build identity. All builds require confirmation in the OTA app before download.
bool isOfficialBuild();
const char* buildKindName();

// Call from the healthy UI loop after setup. A newly booted OTA image is only
// accepted after 30 seconds of responsive runtime; otherwise the bootloader's
// configured rollback support retains the previous valid slot.
void serviceBootHealth(bool setupHealthy);

// Persisted preference defaults to Public. Private and Staging require a
// server-confirmed developer role at selection, retrieval and download.
// Selection failure preserves its actionable reason in lastError().
ReleaseChannel releaseChannel();
bool setReleaseChannel(ReleaseChannel channel);
const char* releaseChannelName(ReleaseChannel channel);

// Authenticate access and fetch the signed channel manifest from the CDN.
// Blocking (~1 s). Returns false on failure.
bool fetchLatestRelease(ReleaseChannel channel, ReleaseInfo* out);
bool isReleaseNewer(ReleaseChannel channel, const char* candidateTag);

// Start a worker task which downloads, hashes and validates the inactive image.
// Call once before pumpFlash().
bool beginFlash(const ReleaseInfo& r);

// Poll worker progress without tying download speed to UI frame rate.
// Returns true while still downloading; false when done (or on error).
// written_out / total_out are updated each call.
bool pumpFlash(int* written_out, int* total_out);

// Activate the already verified image. Call after pumpFlash returns false.
bool finishFlash();

// Safe to call at any time; no-op if flash was not started.
void abortFlash();

// Non-empty after any failure; empty on success.
const char* lastError();

}  // namespace ota_client
}  // namespace services
