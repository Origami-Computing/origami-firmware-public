#include "scene/node.h"

#include "hal/hal_display.h"
#include "scene/asset_cache.h"

namespace scene {

namespace {

static void spriteRender(NodeBase* self, TFT_eSprite& surface, uint32_t now) {
    auto* n = reinterpret_cast<SpriteNode*>(self);
    n->opacity.settle(now);
    n->scaleY.settle(now);
    n->offsetX.settle(now);
    n->offsetY.settle(now);
    // frameAnim settles in Group::settle after children render — not here.
    if (!n->base.visible || !n->contentId) return;

    const uint8_t frame = n->frameAnim.currentFrame(now);
    int pw = 0, ph = 0;
    const uint16_t* px = assetPixelsForFrame(n->contentId, frame, &pw, &ph);
    if (!px) return;

    const int x = n->base.resolvedX + (int)n->offsetX.value(now);
    const int y = n->base.resolvedY + (int)n->offsetY.value(now);
    const float sy = n->scaleY.value(now);
    surface.setSwapBytes(true);
    if (sy >= 0.99f && sy <= 1.01f) {
        surface.pushImage(x, y, pw, ph, px);
    } else {
        const int dh = (int)(ph * sy);
        if (dh < 1) return;
        surface.pushImage(x, y, pw, dh, px);
    }
    surface.setSwapBytes(false);
    if (ph > 0) hal::display::markDirty(y, ph);
}

static bool spriteIsDirty(NodeBase* self, uint32_t now) {
    auto* n = reinterpret_cast<SpriteNode*>(self);
    return n->base.dataDirty || n->opacity.active || n->scaleY.active ||
           n->offsetX.active || n->offsetY.active || n->frameAnim.active;
}

static void spriteSettle(NodeBase* self, uint32_t now) {
    auto* n = reinterpret_cast<SpriteNode*>(self);
    n->opacity.settle(now);
    n->scaleY.settle(now);
    n->offsetX.settle(now);
    n->offsetY.settle(now);
    n->frameAnim.settle(now);
}

}  // namespace

void initSpriteNode(SpriteNode& n) {
    n.base.render  = spriteRender;
    n.base.isDirty = spriteIsDirty;
    n.base.settle  = spriteSettle;
    n.opacity.snap(1.f);
    n.scaleY.snap(1.f);
    n.offsetX.snap(0.f);
    n.offsetY.snap(0.f);
}

}  // namespace scene
