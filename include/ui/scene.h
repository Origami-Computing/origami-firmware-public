#pragma once

#include <stdint.h>
#include <stddef.h>

#include <TB_TFT_eSPI.h>

#include "ui/draw.h"
#include "ui/layout_metrics.h"

class Print;

namespace ui {
namespace scene {

enum class NodeKind : uint8_t {
    Rect,
    Label,
    ProgressBar,
    Circle,
    Line,
    QrCode,
    TileGrid,
    Bitmap,
};

enum class TextAlign : uint8_t {
    TopLeft,
    TopCenter,
    MiddleCenter,
};

// Rect decoration rendered by the shared pixel-dialogue painter. Speech adds
// the tutorial pointer, extending 10 pixels above the rectangle's top edge.
enum class FrameStyle : uint8_t { Plain, Dialogue, Speech };

enum class EditableType : uint8_t {
    Bool,
    Int,
    UInt,
    Color,
    Text,
};

struct EditableProperty {
    const char* nodeId = nullptr;
    const char* name   = nullptr;
    EditableType type  = EditableType::Int;
    void* value        = nullptr;
    int32_t minValue   = 0;
    int32_t maxValue   = 0;
};

struct Node {
    char id[32] = {};
    NodeKind kind  = NodeKind::Rect;
    uint8_t z      = 0;
    bool visible   = true;

    Rect rect = {0, 0, 0, 0};

    uint16_t fillColor   = 0xFFFF;
    uint16_t strokeColor = 0x0000;
    uint8_t  strokeWidth = 0;
    uint8_t  radius      = 0;
    bool     filled      = true;
    FrameStyle frameStyle = FrameStyle::Plain;

    uint16_t fgColor = 0x0000;
    uint16_t bgColor = 0xFFFF;
    uint8_t  font    = 1;
    uint8_t  textSize = 1;
    TextAlign align  = TextAlign::TopLeft;
    bool clearTextBg = false;
    bool scrollOnFocus = false; // Full menu caption; registered only during actual rendering.
    char text[160]   = {};

    uint32_t value = 0;
    uint32_t max   = 1;
    uint16_t trackColor = 0x0000;
    uint16_t fillValueColor = 0x0000;

    const uint16_t* tileColors = nullptr;
    uint16_t tileCount = 0;
    uint8_t tileCols = 0;
    uint8_t tileRows = 0;
    uint8_t tileGap = 0;
    uint16_t emptyColor = 0x0000;
    // Immutable 1-bit MSB-first or 4-bit high-nibble-first coverage data. Source dimensions stay independent
    // of the destination rect so editor resizing never reads past the source.
    const uint8_t* bitmap = nullptr;
    uint16_t bitmapWidth = 0;
    uint16_t bitmapHeight = 0;
    uint8_t bitmapBpp = 1;
    bool bitmapTransparent = false; // 4-bit masks composite over the canvas.
};

class Scene {
public:
    static constexpr uint8_t kMaxNodes = 48;

    void clear();

    Node* addRect(const char* id, Rect rect, uint16_t fillColor,
                  uint8_t radius = 0, uint8_t z = 0);
    Node* addLabel(const char* id, Rect rect, const char* text,
                   uint16_t fgColor, uint16_t bgColor,
                   uint8_t textSize = 1,
                   TextAlign align = TextAlign::TopLeft,
                   uint8_t z = 0);
    Node* addProgressBar(const char* id, Rect rect, uint32_t value, uint32_t max,
                         uint16_t trackColor, uint16_t fillColor,
                         uint8_t radius = 3, uint8_t z = 0);
    Node* addCircle(const char* id, int16_t cx, int16_t cy, int16_t radius,
                    uint16_t strokeColor, uint8_t strokeWidth = 1,
                    uint16_t fillColor = 0, bool filled = false,
                    uint8_t z = 0);
    Node* addLine(const char* id, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  uint16_t color, uint8_t z = 0);
    Node* addQrCode(const char* id, Rect rect, const char* payload,
                    uint8_t moduleSize, uint16_t fgColor, uint16_t bgColor,
                    uint8_t z = 0);
    Node* addTileGrid(const char* id, Rect rect, const uint16_t* tileColors,
                      uint8_t cols, uint8_t rows, uint8_t gap,
                      uint16_t emptyColor, bool fillEmpty = false,
                      uint8_t z = 0);

    void render(TFT_eSPI& surface) const;

    Node* addBitmap(const char* id, Rect rect, const uint8_t* pixels,
                    uint16_t sourceWidth, uint16_t sourceHeight,
                    uint16_t foreground, uint16_t background, uint8_t z = 0,
                    uint8_t bitsPerPixel = 1);

    /** Render with every node shifted by (dx, dy) — used to draw a scene
     *  inside a sub-region (e.g. below the system status bar) without the
     *  producer needing to know the offset. */
    void render(TFT_eSPI& surface, int16_t dx, int16_t dy) const;

    uint8_t count() const { return count_; }
    /** An add was refused for capacity since clear(); existing nodes remain valid. */
    bool overflowed() const { return overflowed_; }
    const Node* nodes() const { return nodes_; }
    Node* find(const char* id);
    const Node* find(const char* id) const;

private:
    Node* addNode(const char* id, NodeKind kind, Rect rect, uint8_t z);

    Node nodes_[kMaxNodes];
    uint8_t count_ = 0;
    bool overflowed_ = false;
};

uint8_t editableProperties(Node& node, EditableProperty* out, uint8_t maxProps);

/** Shared frame-build scene. Use only inside one app's onDraw; render before returning. */
Scene& scratchScene();

/** Runtime/editor-facing current scene. Defaults to scratchScene(). */
void setCurrentScene(Scene* scene);
Scene& currentScene();
void renderCurrentScene(TFT_eSPI& surface);

/** Developer/editor hooks for the most recently built scene. Runtime only. */
void dumpCurrentScene(Print& out);
bool setOverrideFromText(const char* nodeId, const char* property, const char* value);
bool clearOverride(const char* nodeId, const char* property);
void clearOverrides();
uint8_t overrideCount();

class Builder {
public:
    Builder(Scene& scene, int16_t width, int16_t height);

    void clearContent(uint16_t color);
    void panelHeader(const char* text);
    void panelHeader(const char* id, const char* text);
    void statusLine(const char* id, const char* text, uint16_t color);
    void bigNumber(const char* id, uint32_t value, const char* caption);
    void bigNumber(const char* id, const char* valueText, const char* caption);
    void clockDisplay(const char* id, int hour, int minute, bool highlightHour, bool highlightMinute);
    void onOffRow(const char* id, const char* label, bool on, bool selected);
    enum class RowStyle : uint8_t { Standard, Compact, Stacked };
    // A selected form row; null value gives the label the full row width.
    void settingRow(const char* id, const char* label, const char* value, bool selected,
                    RowStyle style = RowStyle::Standard);
    void emptyState(const char* id, const char* first, const char* second = nullptr);
    void labelCenterMuted(const char* id, const char* text);
    void progressBar(const char* id, uint32_t value, uint32_t max);
    void footer(const char* id, const char* text);
    void footerHints(const char* id, const char* first, const char* second);
    void footerHints3(const char* id, const char* first, const char* second, const char* third);
    void spacer(int16_t pixels = layout::space(2));

    int16_t curY() const { return curY_; }

private:
    Scene& scene_;
    int16_t width_;
    int16_t height_;
    int16_t curY_;
};

}  // namespace scene
}  // namespace ui
