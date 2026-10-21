#pragma once

namespace apps {
namespace photos {
namespace policy {

enum class Action { Delete, Escape };

constexpr Action moveAction(Action current, int delta) {
    return delta < 0 ? Action::Delete : delta > 0 ? Action::Escape : current;
}

constexpr int indexAfterRemoval(int index, int remaining) {
    return remaining <= 0 ? 0 : index >= remaining ? remaining - 1 : index;
}

}  // namespace policy
}  // namespace photos
}  // namespace apps
