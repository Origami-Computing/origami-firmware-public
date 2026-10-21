#pragma once

#include "scene/node.h"

namespace scene {

/** Mark every child (and nested group children) dirty — use after full-band erase in onDraw. */
void markGroupDirty(Group& g);

void initGroupNode(Group& g, bool root);

}  // namespace scene
