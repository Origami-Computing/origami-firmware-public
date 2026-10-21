#pragma once

#include <stdint.h>

namespace ui {
namespace scene {

class Scene;

enum SourceLayoutMask : uint32_t {
    kLayoutVisible     = 1u << 0,
    kLayoutX           = 1u << 1,
    kLayoutY           = 1u << 2,
    kLayoutW           = 1u << 3,
    kLayoutH           = 1u << 4,
    kLayoutZ           = 1u << 5,
    kLayoutFillColor   = 1u << 6,
    kLayoutStrokeColor = 1u << 7,
    kLayoutStrokeWidth = 1u << 8,
    kLayoutRadius      = 1u << 9,
    kLayoutFilled      = 1u << 10,
    kLayoutFgColor     = 1u << 11,
    kLayoutBgColor     = 1u << 12,
    kLayoutTextSize    = 1u << 13,
    kLayoutText        = 1u << 14,
    kLayoutValue       = 1u << 15,
    kLayoutMax         = 1u << 16,
    kLayoutTrackColor  = 1u << 17,
};

struct SourceNodeOverride {
    const char* nodeId;
    uint32_t mask;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint8_t z;
    bool visible;
    uint16_t fillColor;
    uint16_t strokeColor;
    uint8_t strokeWidth;
    uint8_t radius;
    bool filled;
    uint16_t fgColor;
    uint16_t bgColor;
    uint8_t textSize;
    const char* text;
    uint32_t value;
    uint32_t max;
    uint16_t trackColor;
};

struct SourceScreenOverrides {
    const char* appId;
    const char* screenId;
    const SourceNodeOverride* nodes;
    uint16_t count;
};

void applySourceLayout(Scene& scene, const char* appId, const char* screenId);

}  // namespace scene
}  // namespace ui
