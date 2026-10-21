#include "ui/clock_layout.h"
#include "ui/scene.h"
#include "ui/layout_metrics.h"
#include "ui/focused_label.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "ui/qr_code.h"
#include "ui/alpha_bitmap.h"
#include "ui/screen_chrome.h"
#include "ui/safe_area_draw.h"
#include "ui/toggle_row_layout.h"
#include "ui/pixel_dialogue_frame.h"

#include <qrcode.h>

namespace ui {
namespace scene {

namespace {

static constexpr uint16_t kProgressFill  = 0x03A0;
static constexpr uint16_t kProgressTrack = kUiPanel;
static constexpr uint8_t kMaxOverrides = 32;

struct RuntimeOverride {
    bool active = false;
    char nodeId[32] = {};
    char property[20] = {};
    char value[160] = {};
};

static RuntimeOverride s_overrides[kMaxOverrides];
static Scene* s_currentScene = nullptr;

static void copyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int textHeight(uint8_t textSize) {
    return 8 * (int)textSize;
}

static bool textEquals(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

static long parseLong(const char* text) {
    if (!text) return 0;
    return strtol(text, nullptr, 0);
}

static bool parseBool(const char* text) {
    if (!text) return false;
    return strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
           strcasecmp(text, "on") == 0 || strcasecmp(text, "yes") == 0;
}

static uint8_t clampU8(long value, uint8_t lo, uint8_t hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return (uint8_t)value;
}

static uint16_t clampU16(long value) {
    if (value < 0) return 0;
    if (value > 0xFFFF) return 0xFFFF;
    return (uint16_t)value;
}

static uint32_t clampU32(long value, uint32_t minValue = 0) {
    if (value < (long)minValue) return minValue;
    return (uint32_t)value;
}

static const char* kindName(NodeKind kind) {
    switch (kind) {
        case NodeKind::Rect:        return "rect";
        case NodeKind::Label:       return "label";
        case NodeKind::ProgressBar: return "progress";
        case NodeKind::Circle:      return "circle";
        case NodeKind::Line:        return "line";
        case NodeKind::QrCode:      return "qr";
        case NodeKind::TileGrid:    return "tilegrid";
        case NodeKind::Bitmap:      return "bitmap";
    }
    return "unknown";
}

static void applyOverride(Node& node, const char* property, const char* value) {
    if (!property || !value) return;

    if (textEquals(property, "visible")) node.visible = parseBool(value);
    else if (textEquals(property, "x")) node.rect.x = (int16_t)parseLong(value);
    else if (textEquals(property, "y")) node.rect.y = (int16_t)parseLong(value);
    else if (textEquals(property, "w")) node.rect.w = (int16_t)parseLong(value);
    else if (textEquals(property, "h")) node.rect.h = (int16_t)parseLong(value);
    else if (textEquals(property, "z")) node.z = clampU8(parseLong(value), 0, 255);
    else if (textEquals(property, "text")) copyText(node.text, sizeof(node.text), value);
    else if (textEquals(property, "fillColor")) {
        if (node.kind == NodeKind::ProgressBar) node.fillValueColor = clampU16(parseLong(value));
        else node.fillColor = clampU16(parseLong(value));
    }
    else if (textEquals(property, "strokeColor")) node.strokeColor = clampU16(parseLong(value));
    else if (textEquals(property, "strokeWidth")) node.strokeWidth = clampU8(parseLong(value), 0, 8);
    else if (textEquals(property, "radius")) node.radius = clampU8(parseLong(value), 0, 32);
    else if (textEquals(property, "filled")) node.filled = parseBool(value);
    else if (textEquals(property, "fgColor")) node.fgColor = clampU16(parseLong(value));
    else if (textEquals(property, "bgColor")) node.bgColor = clampU16(parseLong(value));
    else if (textEquals(property, "textSize")) node.textSize = clampU8(parseLong(value), 1, 6);
    else if (textEquals(property, "value")) node.value = clampU32(parseLong(value));
    else if (textEquals(property, "max")) node.max = clampU32(parseLong(value), 1);
    else if (textEquals(property, "trackColor")) node.trackColor = clampU16(parseLong(value));
    else if (textEquals(property, "fillValueColor") || textEquals(property, "fillColorValue")) {
        node.fillValueColor = clampU16(parseLong(value));
    } else if (textEquals(property, "moduleSize")) {
        node.value = clampU8(parseLong(value), 1, 8);
    } else if (textEquals(property, "tileGap")) {
        node.tileGap = clampU8(parseLong(value), 0, 16);
    } else if (textEquals(property, "emptyColor")) {
        node.emptyColor = clampU16(parseLong(value));
    } else if (textEquals(property, "fillEmpty")) {
        node.filled = parseBool(value);
    }
}

static Node nodeWithOverrides(const Node& source) {
    Node node = source;
    for (uint8_t i = 0; i < kMaxOverrides; i++) {
        const RuntimeOverride& ov = s_overrides[i];
        if (!ov.active) continue;
        if (strcmp(ov.nodeId, node.id) != 0) continue;
        applyOverride(node, ov.property, ov.value);
    }
    return node;
}

static void renderRect(TFT_eSPI& c, const Node& n) {
    if (n.frameStyle != FrameStyle::Plain) {
        pixel_dialogue::draw(c,{n.rect.x,n.rect.y,n.rect.w,n.rect.h},n.strokeColor,n.fillColor);
        if (n.frameStyle == FrameStyle::Speech)
            pixel_dialogue::speechCorner(c,n.rect.x,n.rect.y,n.strokeColor,n.fillColor);
        return;
    }
    if (n.radius > 0) {
        c.fillRoundRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.radius, n.fillColor);
        if (n.strokeWidth > 0) c.drawRoundRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.radius, n.strokeColor);
    } else {
        c.fillRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.fillColor);
        if (n.strokeWidth > 0) c.drawRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.strokeColor);
    }
}

static void renderLabel(TFT_eSPI& c, const Node& n) {
    if (n.clearTextBg) c.fillRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.bgColor);

    c.setTextFont(n.font);
    c.setTextSize(n.textSize);
    c.setTextColor(n.fgColor, n.bgColor);

    const uint8_t datum = n.align == TextAlign::TopCenter ? TC_DATUM :
                          n.align == TextAlign::MiddleCenter ? MC_DATUM : TL_DATUM;
    safe_area::drawLabel(c,n.text,{n.rect.x,n.rect.y,n.rect.w,n.rect.h},datum,n.font);
    if (n.scrollOnFocus) focused_label::observe(c,n.text,n.rect.x,n.rect.y,n.rect.w,n.rect.h,
                                               n.fgColor,n.bgColor,n.font,n.textSize);

    c.setTextDatum(TL_DATUM);
}

static void renderProgressBar(TFT_eSPI& c, const Node& n) {
    const uint32_t max = n.max == 0 ? 1 : n.max;
    int fillW = (int)((uint64_t)n.value * (uint32_t)n.rect.w / max);
    if (fillW < 0) fillW = 0;
    if (fillW > n.rect.w) fillW = n.rect.w;

    c.fillRoundRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.radius, n.trackColor);
    if (fillW > 0) {
        c.fillRoundRect(n.rect.x, n.rect.y, fillW, n.rect.h, n.radius, n.fillValueColor);
    }
}

static void renderCircle(TFT_eSPI& c, const Node& n) {
    const int16_t cx = n.rect.x;
    const int16_t cy = n.rect.y;
    const int16_t r  = n.rect.w;
    if (n.filled) c.fillCircle(cx, cy, r, n.fillColor);
    if (n.strokeWidth > 0) {
        for (uint8_t i = 0; i < n.strokeWidth; i++) {
            c.drawCircle(cx, cy, r - i, n.strokeColor);
        }
    }
}

static void renderLine(TFT_eSPI& c, const Node& n) {
    c.drawLine(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.strokeColor);
}

static void renderQrCode(TFT_eSPI& c, const Node& n) {
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)];
    qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, n.text);

    const uint8_t moduleSize = n.value > 0 ? (uint8_t)n.value : 1;
    ui::renderQrCode(c, &qrcode, n.rect.x, n.rect.y, moduleSize, n.fgColor, n.bgColor);
}

static void renderTileGrid(TFT_eSPI& c, const Node& n) {
    if (!n.tileColors || n.tileCols == 0 || n.tileRows == 0) return;

    const int16_t cellW = n.tileCols > 0 ? n.rect.w / n.tileCols : 0;
    const int16_t cellH = n.tileRows > 0 ? n.rect.h / n.tileRows : 0;
    if (cellW <= 0 || cellH <= 0) return;

    if (n.filled) c.fillRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.emptyColor);

    const int16_t inset = n.tileGap;
    const uint16_t skipColor = n.trackColor;
    for (uint8_t row = 0; row < n.tileRows; row++) {
        for (uint8_t col = 0; col < n.tileCols; col++) {
            const uint16_t idx = (uint16_t)row * n.tileCols + col;
            if (idx >= n.tileCount) return;
            const uint16_t color = n.tileColors[idx];
            if (color == skipColor && !n.filled) continue;
            const uint16_t drawColor = (color == skipColor) ? n.emptyColor : color;
            const int16_t x = n.rect.x + col * cellW + inset;
            const int16_t y = n.rect.y + row * cellH + inset;
            const int16_t w = cellW - inset * 2;
            const int16_t h = cellH - inset * 2;
            if (w > 0 && h > 0) c.fillRect(x, y, w, h, drawColor);
        }
    }
}

static void renderBitmap(TFT_eSPI& c, const Node& n) {
    if (!n.bitmap || !n.bitmapWidth || !n.bitmapHeight || n.rect.w <= 0 || n.rect.h <= 0) return;
    if (n.bitmapBpp == 4) {
        drawAlphaBitmap(c, n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.bitmap,
                        n.bitmapWidth, n.bitmapHeight, n.fgColor, n.bgColor, n.bitmapTransparent);
        return;
    }
    if (n.rect.w == n.bitmapWidth && n.rect.h == n.bitmapHeight) {
        c.drawBitmap(n.rect.x, n.rect.y, n.bitmap, n.bitmapWidth, n.bitmapHeight, n.fgColor, n.bgColor);
        return;
    }
    c.fillRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.bgColor);
    const uint32_t stride = (n.bitmapWidth + 7u) / 8u;
    // Nearest-neighbor scaling in foreground runs; no scratch allocation.
    for (int y = 0; y < n.rect.h; ++y) {
        const uint32_t sourceRow = uint32_t(y) * n.bitmapHeight / n.rect.h * stride;
        int start = -1;
        for (int x = 0; x <= n.rect.w; ++x) {
            const uint32_t sx = uint32_t(x) * n.bitmapWidth / n.rect.w;
            const bool ink = x < n.rect.w && (pgm_read_byte(n.bitmap + sourceRow + sx / 8) & (0x80 >> (sx % 8)));
            if (ink && start < 0) start = x;
            if (!ink && start >= 0) {
                c.drawFastHLine(n.rect.x + start, n.rect.y + y, x - start, n.fgColor);
                start = -1;
            }
        }
    }
}

}  // namespace

void Scene::clear() {
    count_ = 0;
    overflowed_ = false;
}

Node* Scene::addNode(const char* id, NodeKind kind, Rect rect, uint8_t z) {
    if (count_ >= kMaxNodes) { overflowed_ = true; return nullptr; }
    Node& node = nodes_[count_++];
    node = Node{};
    copyText(node.id, sizeof(node.id), id);
    node.kind = kind;
    node.rect = rect;
    node.z = z;
    return &node;
}

Node* Scene::addRect(const char* id, Rect rect, uint16_t fillColor, uint8_t radius, uint8_t z) {
    Node* node = addNode(id, NodeKind::Rect, rect, z);
    if (!node) return nullptr;
    node->fillColor = fillColor;
    node->radius = radius;
    return node;
}

Node* Scene::addLabel(const char* id, Rect rect, const char* text,
                      uint16_t fgColor, uint16_t bgColor,
                      uint8_t textSize, TextAlign align, uint8_t z) {
    Node* node = addNode(id, NodeKind::Label, rect, z);
    if (!node) return nullptr;
    node->fgColor = fgColor;
    node->bgColor = bgColor;
    node->textSize = textSize;
    node->align = align;
    node->clearTextBg = true;
    copyText(node->text, sizeof(node->text), text);
    return node;
}

Node* Scene::addProgressBar(const char* id, Rect rect, uint32_t value, uint32_t max,
                            uint16_t trackColor, uint16_t fillColor,
                            uint8_t radius, uint8_t z) {
    Node* node = addNode(id, NodeKind::ProgressBar, rect, z);
    if (!node) return nullptr;
    node->value = value;
    node->max = max == 0 ? 1 : max;
    node->trackColor = trackColor;
    node->fillValueColor = fillColor;
    node->radius = radius;
    return node;
}

Node* Scene::addCircle(const char* id, int16_t cx, int16_t cy, int16_t radius,
                       uint16_t strokeColor, uint8_t strokeWidth,
                       uint16_t fillColor, bool filled, uint8_t z) {
    Node* node = addNode(id, NodeKind::Circle, {cx, cy, radius, radius}, z);
    if (!node) return nullptr;
    node->strokeColor = strokeColor;
    node->strokeWidth = strokeWidth;
    node->fillColor = fillColor;
    node->filled = filled;
    return node;
}

Node* Scene::addLine(const char* id, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                     uint16_t color, uint8_t z) {
    Node* node = addNode(id, NodeKind::Line, {x0, y0, x1, y1}, z);
    if (!node) return nullptr;
    node->strokeColor = color;
    node->strokeWidth = 1;
    return node;
}

Node* Scene::addQrCode(const char* id, Rect rect, const char* payload,
                       uint8_t moduleSize, uint16_t fgColor, uint16_t bgColor,
                       uint8_t z) {
    Node* node = addNode(id, NodeKind::QrCode, rect, z);
    if (!node) return nullptr;
    copyText(node->text, sizeof(node->text), payload);
    node->value = moduleSize;
    node->fgColor = fgColor;
    node->bgColor = bgColor;
    return node;
}

Node* Scene::addTileGrid(const char* id, Rect rect, const uint16_t* tileColors,
                         uint8_t cols, uint8_t rows, uint8_t gap,
                         uint16_t emptyColor, bool fillEmpty, uint8_t z) {
    Node* node = addNode(id, NodeKind::TileGrid, rect, z);
    if (!node) return nullptr;
    node->tileColors = tileColors;
    node->tileCount = (uint16_t)cols * rows;
    node->tileCols = cols;
    node->tileRows = rows;
    node->tileGap = gap;
    node->emptyColor = emptyColor;
    node->trackColor = emptyColor;
    node->filled = fillEmpty;
    return node;
}

Node* Scene::addBitmap(const char* id, Rect rect, const uint8_t* pixels,
                       uint16_t sourceWidth, uint16_t sourceHeight,
                       uint16_t foreground, uint16_t background, uint8_t z, uint8_t bitsPerPixel) {
    if (!pixels || !sourceWidth || !sourceHeight || (bitsPerPixel != 1 && bitsPerPixel != 4)) return nullptr;
    Node* node = addNode(id, NodeKind::Bitmap, rect, z);
    if (!node) return nullptr;
    node->bitmap = pixels;
    node->bitmapWidth = sourceWidth;
    node->bitmapHeight = sourceHeight;
    node->bitmapBpp = bitsPerPixel;
    node->fgColor = foreground;
    node->bgColor = background;
    return node;
}

void Scene::render(TFT_eSPI& surface) const { render(surface, 0, 0); }

void Scene::render(TFT_eSPI& surface, int16_t dx, int16_t dy) const {
    focused_label::contentPainted(surface);
    bool rendered[kMaxNodes] = {};
    for (uint8_t drawn = 0; drawn < count_; drawn++) {
        int best = -1;
        Node bestNode = {};
        for (uint8_t i = 0; i < count_; i++) {
            if (rendered[i]) continue;
            const Node node = nodeWithOverrides(nodes_[i]);
            if (!node.visible) continue;
            if (best < 0 || node.z < bestNode.z) {
                best = i;
                bestNode = node;
            }
        }
        if (best < 0) return;
        rendered[best] = true;

        Node node = bestNode;
        if (!node.visible) continue;
        // Shift into the target sub-region. rect holds (x,y,w,h) for most kinds;
        // for a Line, (w,h) is the second endpoint (x1,y1), so it moves too.
        if (dx || dy) {
            node.rect.x += dx;
            node.rect.y += dy;
            if (node.kind == NodeKind::Line) {
                node.rect.w += dx;
                node.rect.h += dy;
            }
        }
        // A painted rectangle covering the entire content is the scene's base,
        // including runtime editor overrides. Small panels cannot recolor chrome.
        if (node.kind == NodeKind::Rect && node.filled && node.radius == 0 &&
            node.rect.x <= 0 && node.rect.y <= kSystemStatusBarH &&
            node.rect.x + node.rect.w >= surface.width() &&
            node.rect.y + node.rect.h >= surface.height()) {
            setSceneChromeBackground(node.fillColor, node.rect.y < kSystemStatusBarH);
        }
        // Background fills may extend behind the case. Foreground geometry is
        // laid out inside the measured curve before rasterization, including
        // direct-panel scene renders and Lua scenes rendered with dx/dy.
        const bool background = node.kind == NodeKind::Rect && node.strokeWidth == 0 &&
            node.rect.x <= 0 && node.rect.w >= surface.width();
        if (safe_area::applies(surface) && !background && node.kind != NodeKind::Label &&
            node.kind != NodeKind::QrCode) {
            safe_area::Box box{node.rect.x,node.rect.y,node.rect.w,node.rect.h};
            if (node.kind == NodeKind::Circle) {
                box = {node.rect.x-node.rect.w,node.rect.y-node.rect.w,2*node.rect.w+1,2*node.rect.w+1};
            } else if (node.kind == NodeKind::Line) {
                box = {min(node.rect.x,node.rect.w),min(node.rect.y,node.rect.h),
                       abs(node.rect.w-node.rect.x)+1,abs(node.rect.h-node.rect.y)+1};
            }
            const auto fitted = safe_area::fitBox(box);
            if (node.kind == NodeKind::Circle) {
                const int r = (min(fitted.w,fitted.h)-1)/2;
                node.rect = {(int16_t)(fitted.x+r),(int16_t)(fitted.y+r),(int16_t)r,(int16_t)r};
            } else if (node.kind == NodeKind::Line) {
                const bool right = node.rect.w >= node.rect.x, down = node.rect.h >= node.rect.y;
                node.rect = {(int16_t)(fitted.x+(right?0:fitted.w-1)),
                             (int16_t)(fitted.y+(down?0:fitted.h-1)),
                             (int16_t)(fitted.x+(right?fitted.w-1:0)),
                             (int16_t)(fitted.y+(down?fitted.h-1:0))};
            } else {
                node.rect = {(int16_t)fitted.x,(int16_t)fitted.y,(int16_t)fitted.w,(int16_t)fitted.h};
                node.radius = min(int(node.radius),min(fitted.w,fitted.h)/2);
            }
        }
        switch (node.kind) {
            case NodeKind::Rect:        renderRect(surface, node); break;
            case NodeKind::Label:       renderLabel(surface, node); break;
            case NodeKind::ProgressBar: renderProgressBar(surface, node); break;
            case NodeKind::Circle:      renderCircle(surface, node); break;
            case NodeKind::Line:        renderLine(surface, node); break;
            case NodeKind::QrCode:      renderQrCode(surface, node); break;
            case NodeKind::TileGrid:    renderTileGrid(surface, node); break;
            case NodeKind::Bitmap:      renderBitmap(surface, node); break;
        }
    }
}

Node* Scene::find(const char* id) {
    if (!id) return nullptr;
    for (uint8_t i = 0; i < count_; i++) {
        if (strcmp(nodes_[i].id, id) == 0) return &nodes_[i];
    }
    return nullptr;
}

const Node* Scene::find(const char* id) const {
    if (!id) return nullptr;
    for (uint8_t i = 0; i < count_; i++) {
        if (strcmp(nodes_[i].id, id) == 0) return &nodes_[i];
    }
    return nullptr;
}

uint8_t editableProperties(Node& node, EditableProperty* out, uint8_t maxProps) {
    if (!out || maxProps == 0) return 0;
    uint8_t n = 0;
    auto add = [&](const char* name, EditableType type, void* value, int32_t minValue, int32_t maxValue) {
        if (n >= maxProps) return;
        EditableProperty prop;
        prop.nodeId = node.id;
        prop.name = name;
        prop.type = type;
        prop.value = value;
        prop.minValue = minValue;
        prop.maxValue = maxValue;
        out[n++] = prop;
    };

    add("visible", EditableType::Bool, &node.visible, 0, 1);
    add("x", EditableType::Int, &node.rect.x, -320, 320);
    add("y", EditableType::Int, &node.rect.y, -320, 320);
    add("w", EditableType::Int, &node.rect.w, 0, 320);
    add("h", EditableType::Int, &node.rect.h, 0, 320);
    add("z", EditableType::UInt, &node.z, 0, 255);

    switch (node.kind) {
        case NodeKind::Rect:
            add("fillColor", EditableType::Color, &node.fillColor, 0, 0xFFFF);
            add("strokeColor", EditableType::Color, &node.strokeColor, 0, 0xFFFF);
            add("strokeWidth", EditableType::UInt, &node.strokeWidth, 0, 8);
            add("radius", EditableType::UInt, &node.radius, 0, 16);
            break;
        case NodeKind::Label:
            add("text", EditableType::Text, node.text, 0, (int32_t)sizeof(node.text) - 1);
            add("fgColor", EditableType::Color, &node.fgColor, 0, 0xFFFF);
            add("bgColor", EditableType::Color, &node.bgColor, 0, 0xFFFF);
            add("textSize", EditableType::UInt, &node.textSize, 1, 6);
            break;
        case NodeKind::ProgressBar:
            add("value", EditableType::UInt, &node.value, 0, (int32_t)node.max);
            add("max", EditableType::UInt, &node.max, 1, 1000000);
            add("trackColor", EditableType::Color, &node.trackColor, 0, 0xFFFF);
            add("fillColor", EditableType::Color, &node.fillValueColor, 0, 0xFFFF);
            add("radius", EditableType::UInt, &node.radius, 0, 16);
            break;
        case NodeKind::Circle:
            add("filled", EditableType::Bool, &node.filled, 0, 1);
            add("fillColor", EditableType::Color, &node.fillColor, 0, 0xFFFF);
            add("strokeColor", EditableType::Color, &node.strokeColor, 0, 0xFFFF);
            add("strokeWidth", EditableType::UInt, &node.strokeWidth, 0, 8);
            break;
        case NodeKind::Line:
            add("strokeColor", EditableType::Color, &node.strokeColor, 0, 0xFFFF);
            break;
        case NodeKind::QrCode:
            add("text", EditableType::Text, node.text, 0, (int32_t)sizeof(node.text) - 1);
            add("moduleSize", EditableType::UInt, &node.value, 1, 8);
            add("fgColor", EditableType::Color, &node.fgColor, 0, 0xFFFF);
            add("bgColor", EditableType::Color, &node.bgColor, 0, 0xFFFF);
            break;
        case NodeKind::TileGrid:
            add("tileGap", EditableType::UInt, &node.tileGap, 0, 16);
            add("emptyColor", EditableType::Color, &node.emptyColor, 0, 0xFFFF);
            add("fillEmpty", EditableType::Bool, &node.filled, 0, 1);
            break;
        case NodeKind::Bitmap:
            add("fgColor", EditableType::Color, &node.fgColor, 0, 0xFFFF);
            add("bgColor", EditableType::Color, &node.bgColor, 0, 0xFFFF);
            break;
    }
    return n;
}

Scene& scratchScene() {
    static Scene scene;
    if (!s_currentScene) s_currentScene = &scene;
    return scene;
}

void setCurrentScene(Scene* scene) {
    s_currentScene = scene ? scene : &scratchScene();
}

Scene& currentScene() {
    return s_currentScene ? *s_currentScene : scratchScene();
}

void renderCurrentScene(TFT_eSPI& surface) {
    currentScene().render(surface);
}

void dumpCurrentScene(Print& out) {
    const Scene& scene = currentScene();
    out.printf("[scene] nodes=%u overrides=%u\n", scene.count(), overrideCount());
    if (scene.overflowed()) out.printf("[scene] overflow=1 capacity=%u\n", Scene::kMaxNodes);
    for (uint8_t i = 0; i < scene.count(); i++) {
        const Node node = nodeWithOverrides(scene.nodes()[i]);
        out.printf("[scene] %02u id=%s kind=%s rect=%d,%d,%d,%d z=%u vis=%u",
                   i, node.id, kindName(node.kind), node.rect.x, node.rect.y,
                   node.rect.w, node.rect.h, node.z, node.visible ? 1 : 0);
        switch (node.kind) {
            case NodeKind::Rect:
                out.printf(" fill=0x%04X stroke=0x%04X sw=%u r=%u\n",
                           node.fillColor, node.strokeColor, node.strokeWidth, node.radius);
                if(node.frameStyle!=FrameStyle::Plain) out.printf("[scene-frame] id=%s style=%s\n",node.id,node.frameStyle==FrameStyle::Speech?"speech":"dialogue");
                break;
            case NodeKind::Label:
                out.printf(" text=\"%s\" fg=0x%04X bg=0x%04X size=%u\n",
                           node.text, node.fgColor, node.bgColor, node.textSize);
                break;
            case NodeKind::ProgressBar:
                out.printf(" value=%lu max=%lu track=0x%04X fill=0x%04X r=%u\n",
                           (unsigned long)node.value, (unsigned long)node.max,
                           node.trackColor, node.fillValueColor, node.radius);
                break;
            case NodeKind::Circle:
                out.printf(" fill=0x%04X stroke=0x%04X sw=%u filled=%u\n",
                           node.fillColor, node.strokeColor, node.strokeWidth, node.filled ? 1 : 0);
                break;
            case NodeKind::Line:
                out.printf(" stroke=0x%04X\n", node.strokeColor);
                break;
            case NodeKind::QrCode:
                out.printf(" module=%lu fg=0x%04X bg=0x%04X text=\"%s\"\n",
                           (unsigned long)node.value, node.fgColor, node.bgColor, node.text);
                break;
            case NodeKind::TileGrid:
                out.printf(" cols=%u rows=%u gap=%u empty=0x%04X skip=0x%04X fillEmpty=%u tiles=%u\n",
                           node.tileCols, node.tileRows, node.tileGap, node.emptyColor,
                           node.trackColor, node.filled ? 1 : 0, node.tileCount);
                break;
            case NodeKind::Bitmap:
                out.printf(" source=%u,%u bpp=%u fg=0x%04X bg=0x%04X data=",
                           node.bitmapWidth, node.bitmapHeight, node.bitmapBpp, node.fgColor, node.bgColor);
                if (node.bitmap) {
                    const uint32_t bytes = ((node.bitmapWidth * node.bitmapBpp + 7u) / 8u) * node.bitmapHeight;
                    const char hex[] = "0123456789abcdef";
                    char chunk[65];
                    for (uint32_t offset = 0; offset < bytes; offset += 32) {
                        const uint32_t count = bytes - offset < 32 ? bytes - offset : 32;
                        for (uint32_t j = 0; j < count; ++j) {
                            const uint8_t value = pgm_read_byte(node.bitmap + offset + j);
                            chunk[j * 2] = hex[value >> 4];
                            chunk[j * 2 + 1] = hex[value & 15];
                        }
                        chunk[count * 2] = 0;
                        out.print(chunk);
                    }
                }
                out.println();
                break;
        }
    }
}

bool setOverrideFromText(const char* nodeId, const char* property, const char* value) {
    if (!nodeId || !nodeId[0] || !property || !property[0] || !value) return false;

    int freeIdx = -1;
    for (uint8_t i = 0; i < kMaxOverrides; i++) {
        RuntimeOverride& ov = s_overrides[i];
        if (ov.active && strcmp(ov.nodeId, nodeId) == 0 && strcmp(ov.property, property) == 0) {
            copyText(ov.value, sizeof(ov.value), value);
            return true;
        }
        if (!ov.active && freeIdx < 0) freeIdx = i;
    }
    if (freeIdx < 0) return false;

    RuntimeOverride& ov = s_overrides[freeIdx];
    ov.active = true;
    copyText(ov.nodeId, sizeof(ov.nodeId), nodeId);
    copyText(ov.property, sizeof(ov.property), property);
    copyText(ov.value, sizeof(ov.value), value);
    return true;
}

bool clearOverride(const char* nodeId, const char* property) {
    if (!nodeId || !property) return false;
    for (uint8_t i = 0; i < kMaxOverrides; i++) {
        RuntimeOverride& ov = s_overrides[i];
        if (ov.active && strcmp(ov.nodeId, nodeId) == 0 && strcmp(ov.property, property) == 0) {
            ov.active = false;
            return true;
        }
    }
    return false;
}

void clearOverrides() {
    for (uint8_t i = 0; i < kMaxOverrides; i++) s_overrides[i].active = false;
}

uint8_t overrideCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kMaxOverrides; i++) {
        if (s_overrides[i].active) n++;
    }
    return n;
}

Builder::Builder(Scene& scene, int16_t width, int16_t height)
    : scene_(scene), width_(width), height_(height), curY_(kSystemStatusBarH) {}

void Builder::clearContent(uint16_t color) {
    scene_.addRect("content.bg", {0, kSystemStatusBarH, width_, (int16_t)(height_ - kSystemStatusBarH)}, color, 0, 0);
    curY_ = kSystemStatusBarH;
}

void Builder::panelHeader(const char* text) {
    panelHeader("header.title", text);
}

void Builder::panelHeader(const char* id, const char* text) {
    scene_.addLabel(id, {14, (int16_t)(curY_ + 6), (int16_t)(width_ - 28), 20},
                    text, kUiFg, kUiBg, 2, TextAlign::TopLeft, 2);
    curY_ += 30;
}

void Builder::statusLine(const char* id, const char* text, uint16_t color) {
    scene_.addLabel(id, {10, (int16_t)(curY_ + 3), (int16_t)(width_ - 20), layout::kStatusLineH},
                    text, color, kUiBg, 1, TextAlign::TopLeft, 1);
    curY_ += layout::kStatusLineH;
}

void Builder::bigNumber(const char* id, uint32_t value, const char* caption) {
    char valueText[16];
    snprintf(valueText, sizeof(valueText), "%lu", (unsigned long)value);
    bigNumber(id, valueText, caption);
}

void Builder::bigNumber(const char* id, const char* valueText, const char* caption) {
    const int16_t numY = curY_ + layout::kBigNumberH / 2 - ((caption && caption[0]) ? 10 : 0);

    char valueId[48];
    char captionId[48];
    snprintf(valueId, sizeof(valueId), "%s.value", id ? id : "big");
    snprintf(captionId, sizeof(captionId), "%s.caption", id ? id : "big");

    const uint8_t valueSize = valueText && strlen(valueText) > 3 ? 4 : 6;
    scene_.addLabel(valueId, {0, (int16_t)(numY - 24), width_, 48},
                    valueText ? valueText : "", kUiFg, kUiBg, valueSize,
                    TextAlign::MiddleCenter, 1);
    if (caption && caption[0]) {
        scene_.addLabel(captionId, {0, (int16_t)(numY + 32), width_, 12},
                        caption, kUiMuted, kUiBg, 1, TextAlign::TopCenter, 1);
    }
    curY_ += layout::kBigNumberH;
}

void Builder::clockDisplay(const char* id, int hour, int minute, bool highlightHour, bool highlightMinute) {
    const auto clock = layout::clockLayout(curY_ + 8,
        width_ == safe_area::kWidth && height_ == safe_area::kHeight);
    if (!clock.placed) { curY_ += layout::kClockH; return; }
    char hourText[8];
    char minuteText[8];
    char hourId[48];
    char colonId[48];
    char minuteId[48];

    snprintf(hourText, sizeof(hourText), "%02d", hour);
    snprintf(minuteText, sizeof(minuteText), "%02d", minute);
    snprintf(hourId, sizeof(hourId), "%s.hour", id ? id : "clock");
    snprintf(colonId, sizeof(colonId), "%s.colon", id ? id : "clock");
    snprintf(minuteId, sizeof(minuteId), "%s.minute", id ? id : "clock");

    const char* ids[] = {hourId, colonId, minuteId};
    const char* texts[] = {hourText, ":", minuteText};
    const uint16_t colors[] = {highlightHour ? kUiFg : kUiMuted, kUiMuted,
                               highlightMinute ? kUiFg : kUiMuted};
    for (unsigned i = 0; i < 3; ++i) {
        const auto& b = clock.parts[i];
        scene_.addLabel(ids[i], {(int16_t)b.x, (int16_t)b.y, (int16_t)b.w, (int16_t)b.h},
                        texts[i], colors[i], kUiBg, 4, TextAlign::TopLeft, 1);
    }
    curY_ += layout::kClockH;
}

void Builder::onOffRow(const char* id, const char* label, bool on, bool selected) {
    const auto row = layout::toggleRowLayout(width_, curY_,
        width_ == safe_area::kWidth && height_ == safe_area::kHeight);
    char text[40];
    snprintf(text, sizeof(text), "%s %s", label ? label : "", on ? "ON" : "off");
    const uint16_t bg = selected ? kUiSelBg : kUiBg;
    const uint16_t fg = selected ? kUiSelFg : (on ? kUiFg : kUiMuted);
    char bgId[48];
    char textId[48];
    snprintf(bgId, sizeof(bgId), "%s.bg", id ? id : "onoff");
    snprintf(textId, sizeof(textId), "%s.label", id ? id : "onoff");
    const auto& b = row.background;
    const auto& t = row.label;
    scene_.addRect(bgId, {(int16_t)b.x, (int16_t)b.y, (int16_t)b.w, (int16_t)b.h}, bg, 0, 1);
    scene_.addLabel(textId, {(int16_t)t.x, (int16_t)t.y, (int16_t)t.w, (int16_t)t.h},
                    text, fg, bg, 2, TextAlign::TopLeft, 2);
    curY_ += layout::kOnOffRowH;
}

void Builder::settingRow(const char* id, const char* label, const char* value, bool selected,
                         RowStyle style) {
    const bool compact = style == RowStyle::Compact;
    const bool stacked = style == RowStyle::Stacked;
    const uint16_t bg = selected ? kUiSelBg : kUiBg;
    const uint16_t fg = selected ? kUiSelFg : kUiFg;
    const int16_t textH = compact ? 14 : default_font::height(2);
    const int16_t textY = curY_ + (compact ? 8 : stacked ? 2 : (layout::kRowH-textH)/2);
    const int16_t rowH = stacked ? layout::space(12) : layout::kRowH;
    const uint8_t textSize = compact ? 1 : 2;
    char nodeId[48];
    const char* prefix = id ? id : "setting";
    snprintf(nodeId, sizeof(nodeId), "%s.bg", prefix);
    scene_.addRect(nodeId, {8, curY_, (int16_t)(width_ - 16), (int16_t)(rowH - (stacked ? 1 : 2))}, bg, 0, 1);
    snprintf(nodeId, sizeof(nodeId), "%s.label", prefix);
    scene_.addLabel(nodeId, {14, textY, (int16_t)(value && !stacked ? (compact ? 82 : 90) : width_ - 28), textH},
                    label, fg, bg, textSize, TextAlign::TopLeft, 2);
    if (value) {
        snprintf(nodeId, sizeof(nodeId), "%s.value", prefix);
        scene_.addLabel(nodeId, {(int16_t)(stacked ? 14 : compact ? 104 : 116),
                        (int16_t)(textY+(stacked ? layout::space(6) : 0)),
                        (int16_t)(width_ - (stacked ? 28 : compact ? 118 : 136)), textH},
                        value, fg, bg, textSize, TextAlign::TopLeft, 2);
    }
    curY_ += rowH;
}

void Builder::emptyState(const char* id, const char* first, const char* second) {
    char firstId[48];
    char secondId[48];
    snprintf(firstId, sizeof(firstId), "%s.0", id ? id : "empty");
    snprintf(secondId, sizeof(secondId), "%s.1", id ? id : "empty");
    scene_.addLabel(firstId, {14, (int16_t)(curY_ + 4), (int16_t)(width_ - 28), layout::kLabelH},
                    first, kUiMuted, kUiBg, 1, TextAlign::TopLeft, 1);
    curY_ += layout::kLabelH;
    if (second && second[0]) {
        scene_.addLabel(secondId, {14, (int16_t)(curY_ + 4), (int16_t)(width_ - 28), layout::kLabelH},
                        second, kUiMuted, kUiBg, 1, TextAlign::TopLeft, 1);
        curY_ += layout::kLabelH;
    }
}

void Builder::labelCenterMuted(const char* id, const char* text) {
    scene_.addLabel(id, {0, (int16_t)(curY_ + 3), width_, layout::kLabelH},
                    text, kUiMuted, kUiBg, 1, TextAlign::TopCenter, 1);
    curY_ += layout::kLabelH;
}

void Builder::progressBar(const char* id, uint32_t value, uint32_t max) {
    const int16_t barW = width_ - 48;
    scene_.addProgressBar(id, {24, curY_, barW, layout::kProgressH},
                          value, max, kProgressTrack, kProgressFill, 3, 1);
    curY_ += layout::kProgressPadH;
}

void Builder::footer(const char* id, const char* text) {
    const int16_t y = height_ - layout::kFooter1H;
    scene_.addRect("footer.bg", {0, y, width_, layout::kFooter1H}, kUiBg, 0, 1);
    scene_.addLabel(id, {8, (int16_t)(y + 5), (int16_t)(width_ - 16), layout::kFooter1H},
                    text, kUiMuted, kUiBg, 1, TextAlign::TopLeft, 2);
}

void Builder::footerHints(const char* id, const char* first, const char* second) {
    const int16_t y = height_ - layout::kFooter2H;
    scene_.addRect("footer.bg", {0, y, width_, layout::kFooter2H}, kUiBg, 0, 1);
    char firstId[48];
    char secondId[48];
    snprintf(firstId, sizeof(firstId), "%s.0", id ? id : "footer");
    snprintf(secondId, sizeof(secondId), "%s.1", id ? id : "footer");
    scene_.addLabel(firstId, {12, (int16_t)(y + 2), (int16_t)(width_ - 24), 12},
                    first, kUiMuted, kUiBg, 1, TextAlign::TopLeft, 2);
    scene_.addLabel(secondId, {12, (int16_t)(y + 14), (int16_t)(width_ - 24), 12},
                    second, kUiMuted, kUiBg, 1, TextAlign::TopLeft, 2);
}

void Builder::footerHints3(const char* id, const char* first, const char* second, const char* third) {
    const int16_t y = height_ - layout::kFooter3H;
    scene_.addRect("footer.bg", {0, y, width_, layout::kFooter3H}, kUiBg, 0, 1);
    char firstId[48];
    char secondId[48];
    char thirdId[48];
    snprintf(firstId, sizeof(firstId), "%s.0", id ? id : "footer");
    snprintf(secondId, sizeof(secondId), "%s.1", id ? id : "footer");
    snprintf(thirdId, sizeof(thirdId), "%s.2", id ? id : "footer");
    scene_.addLabel(firstId, {12, (int16_t)(y + 2), (int16_t)(width_ - 24), 12},
                    first, kUiMuted, kUiBg, 1, TextAlign::TopLeft, 2);
    scene_.addLabel(secondId, {12, (int16_t)(y + 14), (int16_t)(width_ - 24), 12},
                    second, kUiMuted, kUiBg, 1, TextAlign::TopLeft, 2);
    scene_.addLabel(thirdId, {12, (int16_t)(y + 26), (int16_t)(width_ - 24), 12},
                    third, kUiMuted, kUiBg, 1, TextAlign::TopLeft, 2);
}

void Builder::spacer(int16_t pixels) {
    curY_ += pixels;
}

}  // namespace scene
}  // namespace ui
