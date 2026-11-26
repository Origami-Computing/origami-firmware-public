#pragma once
#include <stdint.h>

namespace apps {
namespace photos {
namespace policy {

// Rows of the action sheet. Play exists only for videos. Upload is always
// offered, including for items the cloud already has (re-upload); the sheet
// header shows whether it has been uploaded. Delete is last and always confirms.
enum class Action { Play, Upload, Delete };

struct Sheet {
    Action rows[3];
    int count;
    constexpr Action at(int row) const { return rows[row < 0 ? 0 : row >= count ? count - 1 : row]; }
};

constexpr Sheet sheetFor(bool video) {
    return video ? Sheet{{Action::Play, Action::Upload, Action::Delete}, 3}
                 : Sheet{{Action::Upload, Action::Delete, Action::Delete}, 2};
}

// Sheet focus clamps at both ends; a large encoder burst cannot wrap onto Delete.
constexpr int moveRow(int row, int delta, int count) {
    return count <= 0 ? 0
         : row + (delta < 0 ? -1 : delta > 0 ? 1 : 0) < 0 ? 0
         : row + (delta < 0 ? -1 : delta > 0 ? 1 : 0) >= count ? count - 1
         : row + (delta < 0 ? -1 : delta > 0 ? 1 : 0);
}

// Delete confirmation: false = Cancel (the default), true = Delete.
// Turning clockwise moves right onto Delete.
constexpr bool moveConfirm(bool deleteFocused, int delta) {
    return delta > 0 ? true : delta < 0 ? false : deleteFocused;
}

inline int moveIndex(int index, int delta, int count) {
    if (count <= 0) return 0;
    const int64_t next = (int64_t(index) + delta) % count;
    return int(next < 0 ? next + count : next);
}

constexpr int indexAfterRemoval(int index, int remaining) {
    return remaining <= 0 ? 0 : index >= remaining ? remaining - 1 : index;
}

constexpr const char* actionName(Action action) {
    return action == Action::Play ? "play" : action == Action::Upload ? "upload" : "delete";
}

}  // namespace policy
}  // namespace photos
}  // namespace apps
