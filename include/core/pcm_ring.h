#pragma once
/** Caller-owned PCM queue; all operations must be serialized.
 * Playback workers decode/fill and drain this ring in the same task. The ring
 * provides no synchronization: volatile cursors are not a concurrent SPSC API,
 * and sharing a CPU core does not prevent task preemption.
 * Caller supplies buf/cap, resets before use, and checks free() before push().
 * putBack() may restore only samples from the latest pop, with no intervening
 * push/reset: another producer could already have overwritten those slots.
 * Control-task reset, buffer release and worker lifetime require coordination
 * outside this primitive. It does not establish worker quiescence.
 */
#include <stdint.h>

namespace core {
namespace dsp {

struct PcmRing {
    int16_t*     buf;   // caller-allocated; must stay valid for the ring's lifetime
    volatile int wr;    // write cursor
    volatile int rd;    // read cursor
    int          cap;   // capacity in int16_t samples

    int  used()                           const;
    int  free()                           const;
    void push(const int16_t* data, int count);
    int  pop(int16_t* out, int maxCount);
    // Undo a partial pop — rewinds rd by count samples.
    // Use after an I²S short-write to put unwritten samples back.
    void putBack(int count);
    void reset();
};

}  // namespace dsp
}  // namespace core
