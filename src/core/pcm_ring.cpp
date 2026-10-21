/**
 * @file core/pcm_ring.cpp
 * @brief Serialized PCM ring buffer — see include/core/pcm_ring.h.
 */
#include "core/pcm_ring.h"

namespace core {
namespace dsp {

int PcmRing::used() const {
    if (cap <= 0) return 0;
    int u = wr - rd;
    return u >= 0 ? u : u + cap;
}

int PcmRing::free() const {
    if (cap <= 0) return 0;
    return cap - 1 - used();
}

void PcmRing::push(const int16_t* data, int count) {
    if (!buf || cap <= 0) return;
    for (int i = 0; i < count; i++) {
        buf[wr] = data[i];
        wr = (wr + 1) % cap;
    }
}

int PcmRing::pop(int16_t* out, int maxCount) {
    if (!buf || cap <= 0) return 0;
    int n = used();
    if (n > maxCount) n = maxCount;
    for (int i = 0; i < n; i++) {
        out[i] = buf[rd];
        rd = (rd + 1) % cap;
    }
    return n;
}

void PcmRing::putBack(int count) {
    if (!buf || cap <= 0 || count <= 0) return;
    rd = (rd - count + cap) % cap;
}

void PcmRing::reset() {
    wr = 0;
    rd = 0;
}

}  // namespace dsp
}  // namespace core
