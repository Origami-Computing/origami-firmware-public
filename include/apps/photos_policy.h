#pragma once

namespace apps {
namespace photos {
namespace policy {

enum class Action { Delete, Escape, Upload };
constexpr int actionRow(Action action, bool video) {
    return video ? (action == Action::Upload ? 0 : action == Action::Delete ? 1 : 2)
                 : (action == Action::Delete ? 0 : 1);
}
constexpr Action actionAt(int row, bool video) {
    return video ? (row <= 0 ? Action::Upload : row == 1 ? Action::Delete : Action::Escape)
                 : (row <= 0 ? Action::Delete : Action::Escape);
}

constexpr Action moveAction(Action current, int delta, bool video = false) {
    return actionAt(actionRow(current, video) + (delta < 0 ? -1 : delta > 0 ? 1 : 0), video);
}

constexpr int indexAfterRemoval(int index, int remaining) {
    return remaining <= 0 ? 0 : index >= remaining ? remaining - 1 : index;
}

}  // namespace policy
}  // namespace photos
}  // namespace apps
