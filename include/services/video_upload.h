#pragma once
#include <stdint.h>
namespace services { namespace video_upload {
enum class Step { Complete, Progress, Retry, Deferred, Cancelled, Rejected };
struct Result {
    Step step;unsigned httpStatus;uint32_t waitMs;
    const char* reason; // Static, credential-free message; never a server payload.
};
// Runs one bounded upload turn on the admitted UploadQ worker. Owns orchestration
// of saved identity/session, credential renewal, provider transfer and verified
// receipt. A new identity/admission/empty creation may proceed to the first
// PATCH in the same turn, only after durable state saves; at most one PATCH per
// turn. Queue owns scheduling, lifetime attempts and final removal. Never
// deletes media or silently creates missing resume state. Enqueue must first save
// a binding; source remains immutable until this item leaves the queue.
// checkpoint executes synchronously outside SPI locks; caller feeds/yields and
// returns false on stop/cancellation. Resources remain owned until return.
Result advance(const char* queueId,const char* path,bool (*checkpoint)(void*),void* context);
} }
