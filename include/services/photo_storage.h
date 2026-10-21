#pragma once
#include <stddef.h>
#include <stdint.h>

namespace services { namespace photos {
// UI-task photo storage. Naming uses the existing persisted upload sequence;
// prepared paths are not filesystem locks and must be used by one capture flow.
struct Target {
    uint32_t sequence = 0;
    char path[64] = {};
    bool valid() const { return sequence != 0 && path[0] != 0; }
};
bool initialize();
bool available();
Target prepare(); // Empty on invalid naming or an existing destination.
bool write(const Target& target, const uint8_t* jpeg, size_t length);
// Queue failure retains the saved photo for local browsing; no implicit deletion.
bool enqueue(const Target& saved);
} }
