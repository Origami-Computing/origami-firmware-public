#include "apps/photos_policy.h"

using namespace apps::photos::policy;

static_assert(moveAction(Action::Escape, -1) == Action::Delete, "up selects Delete");
static_assert(moveAction(Action::Delete, 1) == Action::Escape, "down selects Esc");
static_assert(moveAction(Action::Delete, -24) == Action::Delete, "clamp at top");
static_assert(moveAction(Action::Escape, 24) == Action::Escape, "clamp at bottom");
static_assert(moveAction(Action::Delete, 0) == Action::Delete, "zero preserves Delete");
static_assert(moveAction(Action::Escape, 0) == Action::Escape, "zero preserves Esc");
static_assert(indexAfterRemoval(0, 159) == 0, "first selects successor");
static_assert(indexAfterRemoval(80, 159) == 80, "middle selects successor");
static_assert(indexAfterRemoval(159, 159) == 158, "last selects predecessor");
static_assert(indexAfterRemoval(0, 0) == 0, "only image leaves empty gallery");

static_assert(moveAction(Action::Escape, -1, true) == Action::Delete, "video up selects Delete");
static_assert(moveAction(Action::Delete, -1, true) == Action::Upload, "video up selects Upload");
static_assert(moveAction(Action::Upload, -1, true) == Action::Upload, "video clamp top");
static_assert(moveAction(Action::Upload, 1, true) == Action::Delete, "video down selects Delete");
static_assert(moveAction(Action::Delete, 1, true) == Action::Escape, "video down selects Esc");
