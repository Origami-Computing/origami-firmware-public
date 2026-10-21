#include "core/conversation_buffer.h"

#include <Arduino.h>
#include <cstring>
#include <esp_heap_caps.h>
#include "config/app_config.h"
#include "core/str_utils.h"

namespace core {

static constexpr uint32_t kPs = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

bool ConversationBuffer::held() const {
    return dispBuf_ != nullptr && wrapBuf_ != nullptr;
}

bool ConversationBuffer::ensure() {
    if (held()) return true;
    release();

    dispCap_ = AGENT_MSG_PART_MAX_BYTES;
    dispBuf_ = (char*)heap_caps_malloc(dispCap_, kPs);
    if (!dispBuf_) { release(); return false; }
    dispBuf_[0] = '\0';

    wrapCap_ = AGENT_MSG_PART_MAX_BYTES;
    wrapBuf_ = (char*)heap_caps_malloc(wrapCap_, kPs);
    if (!wrapBuf_) { release(); return false; }
    wrapBuf_[0] = '\0';

    partCap_ = AGENT_MSG_PART_MAX_BYTES;
    return true;
}

void ConversationBuffer::release() {
    for (int i = 0; i < kMaxParts; ++i) {
        if (parts_[i]) { heap_caps_free(parts_[i]); parts_[i] = nullptr; }
    }
    partCap_   = 0;
    partCount_ = 0;

    if (dispBuf_) { heap_caps_free(dispBuf_); dispBuf_ = nullptr; }
    dispCap_ = 0;

    if (wrapBuf_) { heap_caps_free(wrapBuf_); wrapBuf_ = nullptr; }
    wrapCap_ = 0;
}

bool ConversationBuffer::ensurePartSlot(int idx) {
    if (idx < 0 || idx >= kMaxParts) return false;
    if (parts_[idx]) return true;
    char* p = (char*)heap_caps_malloc(partCap_, kPs);
    if (!p) {
        Serial.printf("[conv] part %d: malloc %u B failed (PSRAM largest=%u KB)\n", idx,
                      (unsigned)partCap_,
                      (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024u));
        return false;
    }
    p[0]       = '\0';
    parts_[idx] = p;
    return true;
}

void ConversationBuffer::clear() {
    for (int i = 0; i < kMaxParts; ++i) {
        if (parts_[i]) parts_[i][0] = '\0';
    }
    partCount_ = 0;
}

bool ConversationBuffer::appendToken(const char* text) {
    if (!text || !text[0] || !ensure()) return false;
    if (partCount_ == 0) partCount_ = 1;
    if (!ensurePartSlot(partCount_ - 1)) return false;
    char*  dst = parts_[partCount_ - 1];
    size_t cur = strlen(dst);
    size_t rem = (cur >= partCap_) ? 0u : (partCap_ - cur - 1u);
    if (rem > 0) strncat(dst, text, rem);
    if (partCap_ > 0) dst[partCap_ - 1] = '\0';
    return true;
}

bool ConversationBuffer::pushMessage(const char* text) {
    if (!text || !text[0] || !ensure()) return false;
    if (partCount_ == 0) {
        if (!ensurePartSlot(0)) return false;
        strncpy(parts_[0], text, partCap_ - 1u);
        parts_[0][partCap_ - 1u] = '\0';
        partCount_ = 1;
        return true;
    }
    if (!ensurePartSlot(partCount_ - 1)) return false;
    char* last = parts_[partCount_ - 1];
    if (last[0] == '\0') {
        strncpy(last, text, partCap_ - 1u);
        last[partCap_ - 1u] = '\0';
        return true;
    }
    if (partCount_ >= kMaxParts) {
        // At capacity — coalesce into last part with separator.
        static const char kSep[] = "\n\n";
        size_t cur = strlen(last);
        if (cur + sizeof(kSep) - 1u < partCap_) {
            memcpy(last + cur, kSep, sizeof(kSep) - 1u);
            cur += sizeof(kSep) - 1u;
            last[cur] = '\0';
        }
        size_t rem = (cur >= partCap_) ? 0u : (partCap_ - cur - 1u);
        if (rem > 0) strncat(last, text, rem);
        last[partCap_ - 1u] = '\0';
        return true;
    }
    if (!ensurePartSlot(partCount_)) return false;
    strncpy(parts_[partCount_], text, partCap_ - 1u);
    parts_[partCount_][partCap_ - 1u] = '\0';
    partCount_++;
    return true;
}

bool ConversationBuffer::removeSegment(int idx) {
    if (idx < 0 || idx >= partCount_) return false;
    // Rotate the removed buffer into the first unused slot so the next
    // segment reuses its allocation.
    char* removed = parts_[idx];
    for (int i = idx; i + 1 < partCount_; ++i) parts_[i] = parts_[i + 1];
    parts_[partCount_ - 1] = removed;
    if (removed) removed[0] = '\0';
    partCount_--;
    return true;
}

const char* ConversationBuffer::segment(int idx) const {
    if (idx < 0 || idx >= partCount_) return "";
    if (!parts_[idx]) return "";
    return parts_[idx];
}

bool ConversationBuffer::anyNonEmpty() const {
    for (int i = 0; i < partCount_; ++i) {
        if (parts_[i] && parts_[i][0]) return true;
    }
    return false;
}

size_t ConversationBuffer::refreshDisplay(int segIdx) {
    if (!dispBuf_) return 0;
    const char* src = segment(segIdx);
    core::str::utf8LossyToAscii(src, dispBuf_, dispCap_);
    return strlen(dispBuf_);
}

size_t ConversationBuffer::displayLen() const {
    return dispBuf_ ? strlen(dispBuf_) : 0u;
}

}  // namespace core
