#pragma once
#include "upload_queue.h"

namespace services { namespace uploads {
// Serialized setup across UI/svc_task; failed attempts remain retryable.
bool ensureInit();
UploadQueue& queue();
// Observe the published singleton without starting storage setup. Null means
// unavailable; a non-null queue remains alive for the firmware lifetime.
UploadQueue* readyQueue();
// svc_task only, inside ServiceWork admission; network acquisition may block.
void tick();
} }
