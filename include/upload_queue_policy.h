#pragma once

#include <stddef.h>
#include <stdint.h>

namespace upload_queue_policy {

static constexpr uint8_t kAttemptsPerTurn = 4;
static constexpr uint8_t kMaxLifetimeAttempts = 20;

// Short delays keep one item's four-attempt turn together without hammering
// the backend. The caller may add up to 10 percent positive jitter.
constexpr uint32_t retryDelayMs(uint8_t attemptsThisTurn) {
    return attemptsThisTurn <= 1 ? 1000u
         : attemptsThisTurn == 2 ? 2000u
         : 4000u;
}

// Called only after a complete four-attempt turn. completedTurns is derived
// from the persisted lifetime attempt count.
constexpr uint32_t turnCooldownMs(uint8_t completedTurns) {
    return completedTurns <= 1 ? 5u * 60u * 1000u
         : completedTurns == 2 ? 15u * 60u * 1000u
         : completedTurns == 3 ? 60u * 60u * 1000u
                               : 6u * 60u * 60u * 1000u;
}

constexpr bool turnExhausted(uint8_t attemptsThisTurn) {
    return attemptsThisTurn >= kAttemptsPerTurn;
}

constexpr bool lifetimeExhausted(uint8_t attempts) {
    return attempts >= kMaxLifetimeAttempts;
}

inline bool deadlinePending(uint32_t now, uint32_t deadline) {
    return deadline != 0 && static_cast<int32_t>(now - deadline) < 0;
}

// Item must expose status, attemptsThisTurn, and nextEligibleAt fields. Keeping
// this primitive hardware-independent lets the on-device unit test exercise the
// same selection code used by UploadQueue.
template <typename Item, typename Status>
int findRunnableIndex(const Item* items, int count, uint32_t now,
                      Status pendingStatus) {
    for (int i = 0; i < count; i++) {
        const Item& item = items[i];
        if (item.status != pendingStatus || item.attemptsThisTurn == 0) continue;
        return deadlinePending(now, item.nextEligibleAt) ? -1 : i;
    }
    for (int i = 0; i < count; i++) {
        const Item& item = items[i];
        if (item.status == pendingStatus &&
            !deadlinePending(now, item.nextEligibleAt)) {
            return i;
        }
    }
    return -1;
}

template <typename Item>
void rotateToBack(Item* items, int count, int index) {
    if (!items || index < 0 || index >= count - 1) return;
    const Item moved = items[index];
    for (int i = index; i < count - 1; i++) {
        items[i] = items[i + 1];
    }
    items[count - 1] = moved;
}

static_assert(retryDelayMs(1) == 1000u, "first retry delay");
static_assert(retryDelayMs(2) == 2000u, "second retry delay");
static_assert(retryDelayMs(3) == 4000u, "third retry delay");
static_assert(turnCooldownMs(1) == 300000u, "first turn cooldown");
static_assert(turnCooldownMs(2) == 900000u, "second turn cooldown");
static_assert(turnCooldownMs(3) == 3600000u, "third turn cooldown");
static_assert(turnCooldownMs(4) == 21600000u, "fourth turn cooldown");

}  // namespace upload_queue_policy
