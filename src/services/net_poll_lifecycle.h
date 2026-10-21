#pragma once

namespace services { namespace net_poll { namespace lifecycle {
// Internal worker/control handshake. RTOS deletion follows quiescence; neither
// quiescence nor a null handle promises that idle has reclaimed the stack.
void run(void*);
void awaitPublication();
bool claimRetirement();
void publishQuiescence();
} } }
