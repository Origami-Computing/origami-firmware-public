#include "ui/source_layout.h"

#include <string.h>

#include "config/ui_source_layout_generated.h"
#include "ui/scene.h"

namespace ui {
namespace scene {

void applySourceLayout(Scene& scene, const char* appId, const char* screenId) {
    if (!appId || !screenId) return;

    for (uint16_t screenIndex = 0; screenIndex < generated_layout::kScreenCount; ++screenIndex) {
        const SourceScreenOverrides& screen = generated_layout::kScreens[screenIndex];
        if (strcmp(screen.appId, appId) != 0 || strcmp(screen.screenId, screenId) != 0) continue;

        for (uint16_t nodeIndex = 0; nodeIndex < screen.count; ++nodeIndex) {
            const SourceNodeOverride& edit = screen.nodes[nodeIndex];
            Node* node = scene.find(edit.nodeId);
            if (!node) continue;

            if (edit.mask & kLayoutVisible) node->visible = edit.visible;
            if (edit.mask & kLayoutX) node->rect.x = edit.x;
            if (edit.mask & kLayoutY) node->rect.y = edit.y;
            if (edit.mask & kLayoutW) node->rect.w = edit.w;
            if (edit.mask & kLayoutH) node->rect.h = edit.h;
            if (edit.mask & kLayoutZ) node->z = edit.z;
            if (edit.mask & kLayoutFillColor) node->fillColor = edit.fillColor;
            if (edit.mask & kLayoutStrokeColor) node->strokeColor = edit.strokeColor;
            if (edit.mask & kLayoutStrokeWidth) node->strokeWidth = edit.strokeWidth;
            if (edit.mask & kLayoutRadius) node->radius = edit.radius;
            if (edit.mask & kLayoutFilled) node->filled = edit.filled;
            if (edit.mask & kLayoutFgColor) node->fgColor = edit.fgColor;
            if (edit.mask & kLayoutBgColor) node->bgColor = edit.bgColor;
            if (edit.mask & kLayoutTextSize) node->textSize = edit.textSize;
            if (edit.mask & kLayoutText) {
                strncpy(node->text, edit.text ? edit.text : "", sizeof(node->text) - 1);
                node->text[sizeof(node->text) - 1] = '\0';
            }
            if (edit.mask & kLayoutValue) node->value = edit.value;
            if (edit.mask & kLayoutMax) node->max = edit.max;
            if (edit.mask & kLayoutTrackColor) node->trackColor = edit.trackColor;
        }
        return;
    }
}

}  // namespace scene
}  // namespace ui
