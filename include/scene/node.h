#pragma once

#include <stdint.h>
#include "scene/anim.h"
#include <TB_TFT_eSPI.h>

namespace scene {

enum class Align : uint8_t { Fixed, Center, PinFar };

struct NodeBase {
    uint8_t z           = 0;
    bool    visible     = true;
    bool    dataDirty   = false;
    Align   alignX      = Align::Fixed;
    Align   alignY      = Align::Fixed;
    int     x           = 0;
    int     y           = 0;
    int     w           = 0;
    int     h           = 0;
    int     resolvedX   = 0;
    int     resolvedY   = 0;
    AnimFloat yAnim;

    void (*render)(NodeBase* self, TFT_eSprite& surface, uint32_t now);
    bool (*isDirty)(NodeBase* self, uint32_t now);
    void (*settle)(NodeBase* self, uint32_t now);

    void markDirty() { dataDirty = true; }
};

void resolveAlign(NodeBase& n, int parentW, int parentH);

struct Group {
    NodeBase  base;
    NodeBase* children[16];
    uint8_t   childCount = 0;
    TFT_eSprite* sprite  = nullptr;
    uint16_t  bgColor    = 0xFFFF;
    AnimFloat opacity;

    void add(NodeBase* node);
    void remove(NodeBase* node);
    void clear();
    void layout(int parentW, int parentH);
    void render(TFT_eSprite& surface, uint32_t now);
};

struct LabelNode {
    NodeBase base;
    char     text[64];
    uint8_t  fontId  = 1;
    uint8_t  textSize = 1;
    uint16_t color    = 0x0000;
    AnimFloat opacity;
};

struct RectNode {
    NodeBase base;
    uint16_t fillColor   = 0x0000;
    uint16_t borderColor = 0x0000;
    uint8_t  borderW     = 0;
    uint8_t  cornerR     = 0;
    AnimFloat opacity;
};

struct SpriteNode {
    NodeBase  base;
    const char* contentId = nullptr;
    FrameAnim frameAnim;
    AnimFloat opacity;
    AnimFloat scaleY;
    AnimFloat offsetX;
    AnimFloat offsetY;
};

void initLabelNode(LabelNode& n);
void initRectNode(RectNode& n);
void initSpriteNode(SpriteNode& n);

}  // namespace scene
