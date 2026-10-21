#pragma once

namespace hal { namespace audio {

// UI-task owned claim while an old audio worker may still access hardware.
// Claims compose; worker-side admission reads the published count atomically.
// Release only after worker acknowledgment. This is not a general audio lease.
class RetirementBlock {
public:
    RetirementBlock() = default;
    RetirementBlock(const RetirementBlock&) = delete;
    RetirementBlock& operator=(const RetirementBlock&) = delete;
    ~RetirementBlock() { release(); }
    void acquire();
    void release();
private:
    bool held_ = false;
};
bool retirementPending();

} }
