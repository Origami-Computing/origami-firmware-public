#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * PSRAM-backed text storage for a Kami voice round-trip.
 *
 * Owns reply segments allocated as needed, plus two buffers from ensure():
 *   parts[]     — up to AGENT_MSG_MAX_PARTS message segments, each AGENT_MSG_PART_MAX_BYTES
 *   dispBuf     — ASCII copy of the current segment for the dialogue box
 *   wrapScratch — scratch for word-wrap line counting
 *
 * Usage:
 *   core::ConversationBuffer conv;
 *   if (!conv.ensure()) { ... }
 *   conv.clear();
 *   // on token: conv.appendToken(text);
 *   // on message: conv.pushMessage(text);
 *   s_tyAgent = conv.refreshDisplay(segmentIndex);
 *   conv.release();
 */
namespace core {

struct ConversationBuffer {
    bool ensure();    // allocate display/wrap buffers; idempotent
    void release();   // free all; idempotent
    bool held() const;

    // ── Segments ──────────────────────────────────────────────────────────────
    void clear();                          // reset all segments to empty
    bool appendToken(const char* text);    // append token to last segment
    bool pushMessage(const char* text);    // new on-screen segment (coalesces at max)
    bool removeSegment(int idx);           // drop one segment; later segments shift down
    static constexpr int maxSegments() { return kMaxParts; }
    const char* segment(int idx) const;
    int  segmentCount() const { return partCount_; }
    bool anyNonEmpty() const;

    // ── Display buffer ────────────────────────────────────────────────────────
    size_t      refreshDisplay(int segIdx); // UTF-8→ASCII of segment; returns strlen
    const char* displayText() const { return dispBuf_; }
    size_t      displayLen()  const;

    // ── Word-wrap scratch ─────────────────────────────────────────────────────
    char*  wrapScratch()     const { return wrapBuf_; }
    size_t wrapScratchSize() const { return wrapCap_; }

private:
    static constexpr int kMaxParts = 16;  // AGENT_MSG_MAX_PARTS

    char*  parts_[kMaxParts] = {};
    size_t partCap_   = 0;
    int    partCount_ = 0;

    char*  dispBuf_ = nullptr;
    size_t dispCap_ = 0;

    char*  wrapBuf_ = nullptr;
    size_t wrapCap_ = 0;

    bool ensurePartSlot(int idx);
};

}  // namespace core
