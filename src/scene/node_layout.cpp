#include "scene/node.h"

namespace scene {

void resolveAlign(NodeBase& n, int parentW, int parentH) {
    switch (n.alignX) {
        case Align::Fixed:
            n.resolvedX = n.x;
            break;
        case Align::Center:
            n.resolvedX = (parentW - n.w) / 2 + n.x;
            break;
        case Align::PinFar:
            n.resolvedX = parentW - n.w - n.x;
            break;
    }
    switch (n.alignY) {
        case Align::Fixed:
            n.resolvedY = n.y;
            break;
        case Align::Center:
            n.resolvedY = (parentH - n.h) / 2 + n.y;
            break;
        case Align::PinFar:
            n.resolvedY = parentH - n.h - n.y;
            break;
    }
}

}  // namespace scene
