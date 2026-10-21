#include "scene/group_node.h"

#include <Arduino.h>

#include "core/memory.h"
#include "hal/hal_display.h"

namespace scene {

namespace {

static void renderChildInto(TFT_eSprite& target, NodeBase* child, uint32_t now) {
    if (!child || !child->visible || !child->render) return;
    child->render(child, target, now);
    child->dataDirty = false;
}

static bool groupIsDirty(NodeBase* self, uint32_t now) {
    auto* g = reinterpret_cast<Group*>(self);
    if (g->base.dataDirty || g->opacity.active) return true;
    for (uint8_t i = 0; i < g->childCount; i++) {
        NodeBase* c = g->children[i];
        if (c && c->isDirty && c->isDirty(c, now)) return true;
    }
    return false;
}

static void groupSettle(NodeBase* self, uint32_t now) {
    auto* g = reinterpret_cast<Group*>(self);
    g->opacity.settle(now);
    for (uint8_t i = 0; i < g->childCount; i++) {
        NodeBase* c = g->children[i];
        if (c && c->settle) c->settle(c, now);
    }
}

static void ensureChildCompositor(Group& g) {
    if (g.sprite || g.childCount == 0 || g.base.w <= 0 || g.base.h <= 0) return;
    g.sprite = core::memory::allocSprite("scene.group", hal::display::tft(), g.base.w, g.base.h);
}

static void markGroupBoundsDirty(const Group& g) {
    const int y = g.base.resolvedY;
    const int h = g.base.h;
    if (h > 0) hal::display::markDirty(y, h);
}

static void groupRenderRoot(NodeBase* self, TFT_eSprite& surface, uint32_t now) {
    auto* g = reinterpret_cast<Group*>(self);
    if (!groupIsDirty(self, now)) return;

    if (g->sprite || (g->childCount > 0 && g->base.w > 0 && g->base.h > 0)) {
        ensureChildCompositor(*g);
    }

    if (g->sprite) {
        g->sprite->fillSprite(g->bgColor);
        for (uint8_t z = 0; z < 16; z++) {
            for (uint8_t i = 0; i < g->childCount; i++) {
                NodeBase* c = g->children[i];
                if (c && c->z == z) renderChildInto(*g->sprite, c, now);
            }
        }
        g->sprite->pushSprite(g->base.resolvedX, g->base.resolvedY);
        markGroupBoundsDirty(*g);
    } else {
        // Root: full-band erase in boot/menu — repaint every visible child while dirty.
        for (uint8_t z = 0; z < 16; z++) {
            for (uint8_t i = 0; i < g->childCount; i++) {
                NodeBase* c = g->children[i];
                if (!c || c->z != z) continue;
                renderChildInto(surface, c, now);
                if (c->h > 0) hal::display::markDirty(c->resolvedY, c->h);
            }
        }
    }

    groupSettle(self, now);
    g->base.dataDirty = false;
}

static bool isGroupNode(const NodeBase* node) {
    return node && node->render == groupRenderRoot;
}

}  // namespace

void markGroupDirty(Group& g) {
    g.base.markDirty();
    for (uint8_t i = 0; i < g.childCount; i++) {
        NodeBase* c = g.children[i];
        if (!c) continue;
        c->markDirty();
        auto* cg = reinterpret_cast<Group*>(c);
        if (isGroupNode(c) && cg->childCount > 0) {
            markGroupDirty(*cg);
        }
    }
}

void initGroupNode(Group& g, bool root) {
    g.base.render  = groupRenderRoot;
    g.base.isDirty = groupIsDirty;
    g.base.settle  = groupSettle;
    g.opacity.snap(1.f);
    if (!root) {
        g.sprite = nullptr;
    }
}

void Group::add(NodeBase* node) {
    if (!node || childCount >= 16) return;
    children[childCount++] = node;
}

void Group::remove(NodeBase* node) {
    for (uint8_t i = 0; i < childCount; i++) {
        if (children[i] == node) {
            for (uint8_t j = i + 1; j < childCount; j++) children[j - 1] = children[j];
            childCount--;
            return;
        }
    }
}

void Group::clear() {
    if (sprite) {
        core::memory::freeSprite(sprite);
    }
    childCount = 0;
}

void Group::layout(int parentW, int parentH) {
    for (uint8_t i = 0; i < childCount; i++) {
        NodeBase* c = children[i];
        if (!c) continue;
        resolveAlign(*c, parentW, parentH);
        if (c->yAnim.active) {
            c->resolvedY += (int)c->yAnim.value(millis());
        }
        auto* cg = reinterpret_cast<Group*>(c);
        if (isGroupNode(c) && cg->childCount > 0 && cg->base.w > 0 && cg->base.h > 0) {
            cg->layout(cg->base.w, cg->base.h);
        }
    }
}

void Group::render(TFT_eSprite& surface, uint32_t now) {
    layout(surface.width(), surface.height());
    groupRenderRoot(&base, surface, now);
}

}  // namespace scene
