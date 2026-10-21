#include "core/health_policy.h"
#include <atomic>

namespace core { namespace health {
namespace {
constexpr unsigned kUploads = 1, kAutomaticUpdate = 2;
// Before navigation initializes there is no root eligible for an OTA prompt.
std::atomic<unsigned> background{kUploads};
}
void publishBackgroundPolicy(RuntimePolicy policy, int depth) {
    const unsigned bits = (policy.allowBackgroundUploads ? kUploads : 0) |
        (depth == 1 && policy.allowAutomaticUpdate ? kAutomaticUpdate : 0);
    background.store(bits);
}
BackgroundPolicy backgroundPolicy() {
    const unsigned bits = background.load();
    return {bool(bits & kUploads), bool(bits & kAutomaticUpdate)};
}
} }
