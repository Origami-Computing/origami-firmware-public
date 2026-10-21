#include "core/agent_slab.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "config/app_config.h"

namespace core {

bool AgentSlab::acquire() {
    if (held()) return true;

    const uint32_t ps8  = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t align  = 16;
    const uint32_t requestedSeconds = AGENT_STREAM_BACKLOG_SECONDS;
    const uint32_t candidates[] = {requestedSeconds, 20u, 12u, 6u};

    // Prefer the configured backlog, but degrade after camera/wake PSRAM
    // fragmentation instead of failing voice capture outright.
    uint32_t lastTriedSeconds = 0;
    for (uint32_t seconds : candidates) {
        if (seconds == 0 || seconds > requestedSeconds || seconds == lastTriedSeconds) continue;
        lastTriedSeconds = seconds;

        const size_t pcmSz = (size_t)AGENT_OUT_RATE * sizeof(int16_t) * seconds;
        const size_t rawSz = pcmSz + align;

        uint8_t* raw = (uint8_t*)heap_caps_malloc(rawSz, ps8);
        if (!raw) raw = (uint8_t*)heap_caps_malloc(rawSz, MALLOC_CAP_SPIRAM);
        if (raw) {
            const size_t mis = ((uintptr_t)raw) % align;
            const size_t pad = mis ? (align - mis) : 0;
            uint8_t* pcmBase = raw + pad;
            if (pcmBase + pcmSz <= raw + rawSz) {
                allocation_ = raw;
                pcm_     = (int16_t*)pcmBase;
                pcmMax_  = pcmSz / sizeof(int16_t);
            } else {
                heap_caps_free(raw);
            }
        }

        if (!held()) {
            pcm_ = (int16_t*)heap_caps_malloc(pcmSz, ps8);
            if (!pcm_) pcm_ = (int16_t*)heap_caps_malloc(pcmSz, MALLOC_CAP_SPIRAM);
            allocation_ = pcm_;
            pcmMax_ = pcmSz / sizeof(int16_t);
        }

        if (held()) {
            Serial.printf("[agent] slab acquire: pcm=%p ringSamples=%lu backlog=%lus (OK, PSRAM largest=%u KB free=%u KB)\n",
                          pcm_, (unsigned long)pcmMax_, (unsigned long)seconds,
                          (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024u),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
            if (seconds < requestedSeconds) {
                Serial.printf("[agent] slab fallback: requested=%lus using=%lus\n",
                              (unsigned long)requestedSeconds, (unsigned long)seconds);
            }
            return true;
        }

        release();
    }

    Serial.printf("[agent] slab acquire: FAIL requested=%lus (PSRAM largest=%u KB free=%u KB)\n",
                  (unsigned long)requestedSeconds,
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024u),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
    return false;
}

void AgentSlab::release() {
    const bool didFree = allocation_ != nullptr;
    if (allocation_) heap_caps_free(allocation_);
    allocation_ = nullptr;
    pcm_ = nullptr;
    pcmMax_ = 0;
    if (didFree) {
        Serial.printf("[agent] slab release: PSRAM free now=%u KB\n",
                      (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024u));
    }
}

}  // namespace core
