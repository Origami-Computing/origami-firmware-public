#pragma once

#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "services/agent_transport.h"

namespace services { namespace agent_transport {

// Single transport producer, single UI consumer. No callbacks or allocation
// under the lock. reset() invalidates queued events from the previous turn.
// This limits queued storage, not total heap (one consumed event may remain live).
class ReplyMailbox {
    struct Node;
public:
    enum class Type { Message, Token, Status, Metadata, Discard };
    enum class Push { Queued, Stale, Full };
    static constexpr size_t kDefaultBudget = 1024u * 1024u;
    class Event {
    public:
        Event() = default;
        ~Event();
        Event(const Event&) = delete;
        Event& operator=(const Event&) = delete;
        Type type() const;
        const char* text() const;
    private:
        friend class ReplyMailbox;
        Node* node_ = nullptr;
    };
    struct Completion {
        Outcome outcome = Outcome::NetworkError;
        uint8_t cancelTag = 0;
        uint32_t sentSamples = 0;
        uint8_t pendingTasks = 0; // requested delegations whose replies arrive later
        char error[96] = {};
    };
    struct Stats { size_t bytes, peakBytes, events; };

    explicit ReplyMailbox(size_t budget = kDefaultBudget) : budget_(budget) {}
    ~ReplyMailbox();
    ReplyMailbox(const ReplyMailbox&) = delete;
    ReplyMailbox& operator=(const ReplyMailbox&) = delete;
    uint32_t epoch();
    uint32_t reset();
    Push push(uint32_t epoch, Type type, const char* text);
    bool finish(uint32_t epoch, const Completion& completion);
    bool pop(Event& event);
    // Completion cannot overtake text. Available exactly once after the queue drains.
    bool takeCompletion(Completion& completion);
    Stats stats();
private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    size_t budget_, bytes_ = 0, peak_ = 0, count_ = 0;
    uint32_t epoch_ = 1;
    bool closed_ = false, completed_ = false;
    Completion completion_;
};

} }
