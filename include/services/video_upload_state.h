#pragma once
#include "core/video_upload_protocol.h"
#include "core/secrets.h"

namespace services { namespace video_upload {
// Capture before enqueue, using one owned backend snapshot. Stable across bearer
// renewal; URL/account-key changes require explicit requeue. No I/O/allocation.
// Backend video routes also require the key and bearer to identify the same user.
// Output is lowercase SHA-256 on success, empty on failure; never log inputs.
bool binding(const core::secrets::BackendConfig& backend,char (&out)[65]);
// Owns the two-slot SD resume record. One serialized caller (UploadQ, or initial
// enqueue while that item is not active); operations block briefly on SD and
// must not run in a painter/tick. No worker, network access or media deletion.
// Caller owns records, preferably in PSRAM. Tokens are deliberately absent.
struct Resume {
    core::video_upload::Identity media;
    // SHA-256 fingerprint of the backend/account configuration captured at enqueue.
    // Never the raw API key or device token. A changed binding needs explicit requeue.
    char backendBinding[65]{};
    char session[37]{};
    uint8_t generation=0;
    char endpoint[384]{},location[512]{};
};
enum class Identify { Ready, Invalid, Unavailable, NoMemory, Cancelled, Changed };
// Upload worker only, before network admission. Hashes a finalized, immutable
// source through 1 KiB SD reads; no network, worker creation or persistence.
// Caller prevents source edits for this operation and later transfer. Size/mtime
// changes are detected, but metadata checks alone cannot prove file immutability.
// checkpoint runs on the caller task without the SPI lock before each read and
// before publication: feed/yield there; false cancels. No reentry or state edits.
// record changes only on Ready. A previously hashed record is verified, never
// rebound to different content. Save the result before network admission.
Identify identify(const char* path,Resume& record,bool (*checkpoint)(void*),void* context);
// An unhashed initial record has bytes==0 and an empty sha256, but a valid name
// and backendBinding. It cannot have a session or provider URL. The worker fills
// its media identity before network admission; thereafter identity is immutable.
enum class Load { Ready, Missing, Corrupt, Unavailable, NoMemory, InvalidId };
enum class Save { Saved, Invalid, Conflict, Corrupt, Unavailable, NoMemory };
// Output is usable only on Ready. Selects the newest valid slot before returning
// it; callers must compare its binding against the current backend snapshot.
Load load(const char* queueId,Resume& out);
// Alternates slots, closes and reads back the written record before success.
// Preserves the prior valid slot on short write/readback failure. This is not an
// atomic filesystem or an SD power-loss guarantee. Refuses identity/backend/
// session rebinding and backwards/skipped generations; an increment clears URL.
Save save(const char* queueId,const Resume& record);
// Explicit queue removal only. Deletes resume metadata, never the source video.
// Best effort across the two files; false means cleanup was incomplete.
bool erase(const char* queueId);
} }
