#include "scene/node.h"

#include "config/ui_theme.h"

namespace scene {

static void labelRender(NodeBase* self, TFT_eSprite& surface, uint32_t now) {
    auto* n = reinterpret_cast<LabelNode*>(self);
    n->opacity.settle(now);
    if (!n->base.visible) return;
    surface.setTextFont(n->fontId);
    surface.setTextSize(n->textSize);
    surface.setTextDatum(TL_DATUM);
    surface.setTextColor(n->color, kUiBg);
    surface.drawString(n->text, n->base.resolvedX, n->base.resolvedY);
}

static bool labelIsDirty(NodeBase* self, uint32_t now) {
    auto* n = reinterpret_cast<LabelNode*>(self);
    return n->base.dataDirty || n->opacity.active;
}

static void labelSettle(NodeBase* self, uint32_t now) {
    reinterpret_cast<LabelNode*>(self)->opacity.settle(now);
}

void initLabelNode(LabelNode& n) {
    n.base.render  = labelRender;
    n.base.isDirty = labelIsDirty;
    n.base.settle  = labelSettle;
    n.opacity.snap(1.f);
}

}  // namespace scene
