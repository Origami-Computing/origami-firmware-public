#include "hal/audio_retirement.h"
#include <atomic>

namespace hal { namespace audio {
namespace { std::atomic<unsigned> retiring{0}; }
void RetirementBlock::acquire() {
    if (held_) return;
    retiring.fetch_add(1, std::memory_order_release);
    held_ = true;
}
void RetirementBlock::release() {
    if (!held_) return;
    retiring.fetch_sub(1, std::memory_order_release);
    held_ = false;
}
bool retirementPending() { return retiring.load(std::memory_order_acquire) != 0; }
} }
