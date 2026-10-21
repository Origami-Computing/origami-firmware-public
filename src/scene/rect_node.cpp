#include "scene/node.h"

namespace scene {

static void rectRender(NodeBase* self, TFT_eSprite& surface, uint32_t now) {
    auto* n = reinterpret_cast<RectNode*>(self);
    n->opacity.settle(now);
    if (!n->base.visible) return;
    const int x = n->base.resolvedX;
    const int y = n->base.resolvedY;
    const int w = n->base.w;
    const int h = n->base.h;
    if (n->cornerR > 0) {
        surface.fillRoundRect(x, y, w, h, n->cornerR, n->fillColor);
        if (n->borderW > 0) surface.drawRoundRect(x, y, w, h, n->cornerR, n->borderColor);
    } else {
        surface.fillRect(x, y, w, h, n->fillColor);
        if (n->borderW > 0) surface.drawRect(x, y, w, h, n->borderColor);
    }
}

static bool rectIsDirty(NodeBase* self, uint32_t now) {
    auto* n = reinterpret_cast<RectNode*>(self);
    return n->base.dataDirty || n->opacity.active;
}

static void rectSettle(NodeBase* self, uint32_t now) {
    reinterpret_cast<RectNode*>(self)->opacity.settle(now);
}

void initRectNode(RectNode& n) {
    n.base.render  = rectRender;
    n.base.isDirty = rectIsDirty;
    n.base.settle  = rectSettle;
    n.opacity.snap(1.f);
}

}  // namespace scene
