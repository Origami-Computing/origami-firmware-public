#pragma once
#include <stdint.h>

namespace services { namespace onboarding_progress { namespace legacy {
// Read-only guide_v3 migration. The active tutorial uses onboarding::Flow.
enum class Status : uint8_t { New, InProgress, Skipped, Complete };
constexpr uint8_t kTourPages = 12;
struct Progress {
    Status status = Status::New;
    uint8_t checkpoint = 0;
    bool supported = true;
};
constexpr uint32_t kRecordTag = 0x4B030000u; // v3: gated controls + first-class setup
inline bool shouldOffer(const Progress& p) {
    return p.supported && (p.status == Status::New || p.status == Status::InProgress);
}
inline Progress decode(uint32_t word) {
    if (!word) return {};
    Progress p;
    if ((word & 0xFFFF0000u) != kRecordTag ||
        ((word >> 8) & 255) > uint32_t(Status::Complete) || (word & 255) >= kTourPages) {
        p.status = Status::Skipped;
        p.supported = false;
        return p;
    }
    p.status = Status((word >> 8) & 255);
    p.checkpoint = word & 255;
    return p;
}
}}} // namespace services::onboarding_progress::legacy
