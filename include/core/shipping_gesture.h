#pragma once
#include <stdint.h>

namespace core { namespace shipping {
// Physical A/B/C bits 1/2/4; any other bit invalidates the attempt.
// Hold A, B, C in order for 0.8-2 seconds each, releasing between holds.
// No persistent state or hardware: one bounded attempt per wake.
class Gesture {
public:
    enum class Result { Pending, Accepted, Rejected };
    explicit Gesture(uint32_t now) : started_(now), edge_(now), candidateAt_(now) {}
    Result update(uint8_t mask, uint32_t now, bool valid = true) {
        if (result_ != Result::Pending) return result_;
        if (!valid || now-started_ > 12000u || now-edge_ > 2200u ||
            (mask && mask!=1 && mask!=2 && mask!=4)) return reject();
        if (mask != candidate_) { candidate_=mask; candidateAt_=now; }
        if (now-candidateAt_ < 35u || candidate_==stable_) return result_;
        stable_=candidate_;
        if (phase_==0) {
            static const uint8_t order[3]={1,2,4};
            if (stable_!=order[index_] || (index_ && now-edge_<100u)) return reject();
            phase_=1;
        } else {
            if (stable_ || now-edge_<800u || now-edge_>2000u) return reject();
            if (++index_==3) return result_=Result::Accepted;
            phase_=0;
        }
        edge_=now;
        return result_;
    }
private:
    Result reject() { return result_=Result::Rejected; }
    uint32_t started_, edge_, candidateAt_;
    uint8_t candidate_=0, stable_=0, phase_=0, index_=0;
    Result result_=Result::Pending;
};
} }
