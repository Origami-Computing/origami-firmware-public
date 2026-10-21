#include "services/agent_reply_mailbox.h"
#include <cstring>
#include <esp_heap_caps.h>

namespace services { namespace agent_transport {
struct ReplyMailbox::Node {
    Node* next;
    size_t bytes;
    Type type;
    const char* text() const { return reinterpret_cast<const char*>(this + 1); }
};
ReplyMailbox::Event::~Event() { heap_caps_free(node_); }
ReplyMailbox::Type ReplyMailbox::Event::type() const { return node_->type; }
const char* ReplyMailbox::Event::text() const { return node_ ? node_->text() : ""; }
ReplyMailbox::~ReplyMailbox() { reset(); }
uint32_t ReplyMailbox::epoch() {
    portENTER_CRITICAL(&mux_);
    const auto result = epoch_;
    portEXIT_CRITICAL(&mux_);
    return result;
}
uint32_t ReplyMailbox::reset() {
    portENTER_CRITICAL(&mux_);
    Node* old = head_;
    head_ = tail_ = nullptr;
    bytes_ = count_ = peak_ = 0;
    closed_ = completed_ = false;
    const auto result = ++epoch_;
    portEXIT_CRITICAL(&mux_);
    while (old) { Node* next = old->next; heap_caps_free(old); old = next; }
    return result;
}
ReplyMailbox::Push ReplyMailbox::push(uint32_t epoch, Type type, const char* text) {
    if (!text) text = "";
    // Bounded scan also avoids arithmetic overflow on malformed oversized input.
    const size_t length = strnlen(text, budget_);
    if (budget_ <= sizeof(Node) || length >= budget_ - sizeof(Node)) return Push::Full;
    const size_t bytes = sizeof(Node) + length + 1;
    portENTER_CRITICAL(&mux_);
    const bool stale = epoch != epoch_ || closed_;
    const bool full = bytes > budget_ - bytes_;
    portEXIT_CRITICAL(&mux_);
    if (stale) return Push::Stale;
    if (full) return Push::Full;
    auto* node = static_cast<Node*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!node) return Push::Full;
    node->next = nullptr;
    node->bytes = bytes;
    node->type = type;
    memcpy(node + 1, text, length + 1);
    portENTER_CRITICAL(&mux_);
    // reset/finish may have occurred during allocation. Never publish into a new turn.
    const bool stillCurrent = epoch == epoch_ && !closed_;
    if (stillCurrent) {
        if (tail_) tail_->next = node; else head_ = node;
        tail_ = node;
        bytes_ += bytes;
        ++count_;
        if (bytes_ > peak_) peak_ = bytes_;
    }
    portEXIT_CRITICAL(&mux_);
    if (!stillCurrent) { heap_caps_free(node); return Push::Stale; }
    return Push::Queued;
}
bool ReplyMailbox::finish(uint32_t epoch, const Completion& completion) {
    portENTER_CRITICAL(&mux_);
    const bool accepted = epoch == epoch_ && !closed_;
    if (accepted) {
        completion_ = completion;
        completion_.error[sizeof(completion_.error)-1] = '\0';
        closed_ = completed_ = true;
    }
    portEXIT_CRITICAL(&mux_);
    return accepted;
}
bool ReplyMailbox::pop(Event& event) {
    heap_caps_free(event.node_);
    event.node_ = nullptr;
    portENTER_CRITICAL(&mux_);
    if (head_) {
        event.node_ = head_;
        head_ = head_->next;
        if (!head_) tail_ = nullptr;
        bytes_ -= event.node_->bytes;
        --count_;
    }
    portEXIT_CRITICAL(&mux_);
    return event.node_ != nullptr;
}
bool ReplyMailbox::takeCompletion(Completion& completion) {
    portENTER_CRITICAL(&mux_);
    const bool available = completed_ && !head_;
    if (available) { completion = completion_; completed_ = false; }
    portEXIT_CRITICAL(&mux_);
    return available;
}
ReplyMailbox::Stats ReplyMailbox::stats() {
    portENTER_CRITICAL(&mux_);
    const Stats result{bytes_, peak_, count_};
    portEXIT_CRITICAL(&mux_);
    return result;
}
} }
