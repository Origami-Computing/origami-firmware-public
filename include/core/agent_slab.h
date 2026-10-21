#pragma once

#include <stddef.h>
#include <stdint.h>

/** Owns the Agent PCM backlog in PSRAM. Acquire lazily for capture and release
 * after transport finishes. Resource coordination belongs to agent_voice;
 * this allocator does not stop cameras, discard sprites or acquire hardware.
 * The configured backlog and smaller PSRAM fallbacks remain available on N16R8.
 */
namespace core {

struct AgentSlab {
    AgentSlab() = default;
    AgentSlab(const AgentSlab&) = delete;
    AgentSlab& operator=(const AgentSlab&) = delete;
    bool acquire();  // allocate PCM; idempotent if already held
    void release();  // free; idempotent if not held

    int16_t* pcm()     const { return pcm_; }
    uint32_t pcmMax()  const { return pcmMax_; }
    bool     held()    const { return pcm_ != nullptr; }

private:
    int16_t* pcm_     = nullptr;
    uint32_t pcmMax_  = 0;
    void* allocation_ = nullptr; // Own allocation; pcm_ may be an aligned interior view.
};

}  // namespace core
