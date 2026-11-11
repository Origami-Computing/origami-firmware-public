#pragma once
#include "services/video_upload_state.h"
namespace services { namespace video_upload {
enum class BackendOperation { Admit, Status, Complete, Recover };
enum class BackendOutcome { Reply, HttpError, Retry, Cancelled, Invalid, NoMemory };
struct BackendResult { BackendOutcome outcome; unsigned httpStatus; };
// Same worker/reservation/cancellation contract as transfer. HTTPS only. Sends
// the owned backend snapshot's key + bearer only to its configured origin; the
// snapshot must match record.backendBinding. No redirects, persistence or media
// mutation. Output is usable only on Reply and remains subject to its state:
// only State::Complete carries a verified receipt. Status/Recover permit at most the
// next generation; callers save it with the old provider URL cleared.
BackendResult request(BackendOperation operation,const core::secrets::BackendRequest& backend,
                      const Resume& record,core::video_upload::Response& output,
                      bool (*checkpoint)(void*),void* context);
enum class Transfer { Progress, Verify, Uncertain, Retry, Cancelled, Invalid, Storage, NoMemory };
struct TransferResult { Transfer outcome; unsigned httpStatus; };
// One admitted UploadQ worker only; caller owns network reservation and immutable
// source. Inputs must come from the validated backend response and saved resume
// state. Uses only scoped storage credentials, never backend credentials.
// Reload persisted resume state before each call (also after save/readback errors).
// The operation verifies the supplied record against storage before networking.
// Performs empty creation + durable URL save, or HEAD + at most one 6 MiB PATCH.
// HEAD may retain its TLS connection for PATCH on the identical URL; a closed
// socket reconnects normally. Credentials never cross storage origins.
// Uncertain means all final bytes were submitted but the acknowledgment was
// lost; the caller must ask the backend to verify, never mark completion.
// Verify follows authoritative HEAD or an exact final PATCH acknowledgment.
// It means storage bytes are present, NOT a receipt or queue completion.
// PATCH uses 8 KiB PSRAM reads / at most 8 KiB TLS writes and bounded DMA
// pressure waits. Persistent pressure returns NoMemory; cancellation wins.
// No worker creation, media deletion or automatic URL/session recovery. Retry
// preserves the saved URL; next call recovers authoritative offset using HEAD.
// checkpoint runs synchronously outside SPI locks; feed/yield and return false
// to cancel. TLS/connect/write calls can delay acknowledgement up to their timeout.
TransferResult transfer(const char* queueId,const char* path,Resume& record,
                        const core::video_upload::Response& admission,
                        bool (*checkpoint)(void*),void* context);
} }
