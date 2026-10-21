#include "apps/safe_area_app.h"
#ifdef ORIGAMI_DEV_SERIAL_INPUT

#include <Arduino.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "apps/agent_app.h"
#include "core/store.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "services.h"
#include "ui/safe_area_geometry.h"
#include "config/display_safe_area.h"

namespace apps { namespace safe_area {
namespace {
namespace geom = ui::safe_area;
using geom::Config;
using geom::Rect;
Config s_config = geom::defaults();
Config s_saved = geom::defaults();
bool s_loaded = false, s_hasSaved = false, s_dirty = true;
int s_selected = 0;
struct Edit { Config config; int selected; };
constexpr int kHistoryLimit = 32;
Edit s_undo[kHistoryLimit], s_redo[kHistoryLimit];
int s_undoCount = 0, s_redoCount = 0;
bool apply(const Config& c);
const char* const kFields[] = {"left", "top", "right", "bottom", "radius", "padding"};
// Fixed-width, versioned, single-key NVS record. Only explicit save writes it.
struct Record { uint16_t version, width, height, left, top, right, bottom, radius, padding; };
static_assert(sizeof(Record) == 18, "Stable calibration record layout");

int* field(Config& c, int i) {
    switch (i) {
        case 0: return &c.left; case 1: return &c.top;
        case 2: return &c.right; case 3: return &c.bottom;
        case 4: return &c.radius; default: return &c.padding;
    }
}
bool same(const Config& a, const Config& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom && a.radius == b.radius && a.padding == b.padding;
}
void pushHistory(Edit* history, int& count) {
    if (count == kHistoryLimit) {
        for (int i = 1; i < count; ++i) history[i-1] = history[i];
        --count;
    }
    history[count++] = {s_config, s_selected};
}
bool restoreHistory(bool redo) {
    Edit* source = redo ? s_redo : s_undo;
    int& count = redo ? s_redoCount : s_undoCount;
    if (!count) {
        Serial.printf("[safe-area] ERROR nothing to %s\n", redo ? "redo" : "undo");
        return false;
    }
    pushHistory(redo ? s_undo : s_redo, redo ? s_undoCount : s_redoCount);
    const Edit edit = source[--count];
    s_config = edit.config;
    s_selected = edit.selected;
    s_dirty = true;
    services::notifyUserActivity();
    return true;
}
bool loadSaved(bool remember = true) {
    Record r{};
    if (!core::store::getBlob("ui_safe_area", "profile_v1", &r, sizeof(r))) return false;
    const Config c{r.left, r.top, r.right, r.bottom, r.radius, r.padding};
    if (r.version != 1 || r.width != geom::kWidth || r.height != geom::kHeight || !geom::valid(c)) return false;
    if (remember) apply(c);
    else s_config = c;
    s_saved = c;
    s_hasSaved = true;
    s_dirty = true;
    return true;
}
void ensureLoaded() {
    if (s_loaded) return;
    s_loaded = true;
    loadSaved(false);
}
void printStatus() {
    const Rect r = geom::opening(s_config);
    Serial.printf("[safe-area] active=%u left=%d top=%d right=%d bottom=%d radius=%d padding=%d saved=%u modified=%u selected=%s undo=%d redo=%d\n",
        unsigned(isActive()), s_config.left, s_config.top, s_config.right, s_config.bottom,
        s_config.radius, s_config.padding, unsigned(s_hasSaved),
        unsigned(!s_hasSaved || !same(s_config, s_saved)), kFields[s_selected], s_undoCount, s_redoCount);
    Serial.printf("[safe-area] canvas=280x240 opening=(%d,%d)-(%d,%d) size=%dx%d right/bottom=exclusive\n",
        r.x, r.y, r.x+r.w, r.y+r.h, r.w, r.h);
    Serial.printf("[safe-area] replay: safearea set %d %d %d %d %d %d\n",
        s_config.left, s_config.top, s_config.right, s_config.bottom, s_config.radius, s_config.padding);
}
void help() {
    Serial.println("[safe-area] safearea on|off|status|help|save|load|reset|undo|redo|next|prev");
    Serial.println("[safe-area] safearea set LEFT TOP RIGHT BOTTOM RADIUS [PADDING]");
    Serial.println("[safe-area] safearea FIELD N     absolute pixels (left/top/right/bottom/radius/padding)");
    Serial.println("[safe-area] safearea FIELD +N|-N relative; + moves an edge INWARD");
    Serial.println("[safe-area] dial turns adjust; press=next field, LB=previous field, RB=undo, A+B=exit");
    Serial.println("[safe-area] reopen from main menu > Screen Fit; undo/redo keep up to 32 edits until reboot");
    Serial.println("[safe-area] white=case opening, cyan=content padding, red=hidden region; reset is RAM-only");
}
bool apply(const Config& c) {
    if (!geom::valid(c)) {
        Serial.println("[safe-area] ERROR unchanged: insets >=0, opening >=64x64, radius <=half shorter side, padding 0..24");
        return false;
    }
    if (!same(c, s_config)) {
        pushHistory(s_undo, s_undoCount);
        s_redoCount = 0;
        s_config = c;
    }
    s_dirty = true;
    services::notifyUserActivity();
    return true;
}
// Fill exact pixel-center rounded geometry with horizontal runs. This avoids
// driver-specific radius clamping and makes the one-pixel boundary reproducible.
void fill(TFT_eSprite& canvas, const Rect& r, uint16_t color) {
    for (int y = r.y; y < r.y+r.h; ++y) {
        int x = r.x;
        while (x < r.x+r.w && !geom::contains(r, x, y)) ++x;
        if (x < r.x+r.w) canvas.drawFastHLine(x, y, r.w-2*(x-r.x), color);
    }
}
void label(TFT_eSprite& canvas, const char* text, int x, int y, uint16_t color) {
    canvas.setTextColor(color, TFT_BLACK);
    canvas.drawString(text, x, y, 1);
}
void draw(TFT_eSprite& canvas) {
    if (!s_dirty) return;
    s_dirty = false;
    canvas.resetViewport();
    canvas.fillScreen(0x3000); // muted red: expected to be behind the retaining lip
    const Rect r = geom::opening(s_config);
    fill(canvas, r, TFT_WHITE);
    fill(canvas, geom::inset(r, 1), TFT_BLACK);
    if (s_config.padding > 0) {
        fill(canvas, geom::inset(r, s_config.padding), TFT_CYAN);
        fill(canvas, geom::inset(r, s_config.padding+1), TFT_BLACK);
    }
    // Keep annotations away from the measured boundary, even for extreme fits.
    // A rectangular label block is shown only if all four corners fit inside.
    const int cx = r.x+r.w/2, cy = r.y+r.h/2;
    const Rect labels = geom::inset(r, s_config.padding+3);
    if (geom::contains(labels, cx-102, cy-81) && geom::contains(labels, cx+102, cy-81) &&
        geom::contains(labels, cx-102, cy+85) && geom::contains(labels, cx+102, cy+85)) {
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextSize(1);
        canvas.setTextPadding(0);
        label(canvas, "TOP", cx, cy-81, TFT_YELLOW);
        label(canvas, "SCREEN / CASE FIT", cx, cy-65, TFT_WHITE);
        char text[48];
        snprintf(text, sizeof(text), "L %d  T %d  R %d  B %d", s_config.left, s_config.top, s_config.right, s_config.bottom);
        label(canvas, text, cx, cy-48, TFT_WHITE);
        snprintf(text, sizeof(text), "Radius %d   Padding %d", s_config.radius, s_config.padding);
        label(canvas, text, cx, cy-35, TFT_CYAN);
        label(canvas, "White: fit the case opening", cx, cy-17, TFT_WHITE);
        label(canvas, "Cyan: keep text/icons inside", cx, cy-5, TFT_CYAN);
        snprintf(text, sizeof(text), "Dial: %s = %d", kFields[s_selected], *field(s_config, s_selected));
        label(canvas, text, cx, cy+14, TFT_YELLOW);
        label(canvas, "Press dial: next field", cx, cy+30, TFT_WHITE);
        label(canvas, "Left: previous field", cx, cy+42, TFT_WHITE);
        snprintf(text, sizeof(text), "Right: undo (%d)   A+B: exit", s_undoCount);
        label(canvas, text, cx, cy+54, TFT_WHITE);
        label(canvas, "Serial: safearea save", cx, cy+67, TFT_WHITE);
        label(canvas, "BOTTOM", cx, cy+78, TFT_YELLOW);
    }
    canvas.setTextDatum(TL_DATUM);
    hal::display::markAllDirty();
}
void enter() {
    ensureLoaded();
    s_dirty = true;
    services::notifyUserActivity();
}
void tick(uint32_t) { services::notifyUserActivity(); } // keep the fitting target awake
void input(hal::input::InputEvent ev) {
    using ET = hal::input::EventType;
    if (ev.type == ET::ButtonB_Tap) {
        if (restoreHistory(false)) printStatus();
    } else if (ev.type == ET::ButtonA_Tap || ev.type == ET::ButtonC_Tap) {
        s_selected = (s_selected + (ev.type == ET::ButtonA_Tap ? 5 : 1))%6;
        s_dirty = true;
        printStatus();
    } else if (ev.type == ET::Scroll) {
        Config next = s_config;
        *field(next, s_selected) += ev.delta;
        if (apply(next)) printStatus();
    }
}
const ::scene::Screen kScreen = {"Screen Fit", enter, nullptr, tick, input, nullptr, draw, "calibration", nullptr};
const ::scene::Screen* const kScreens[] = {&kScreen};

bool integer(const char* token, int& value) {
    if (!token || !token[0]) return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = strtol(token, &end, 10);
    if (errno || end == token || *end || parsed < -1000 || parsed > 1000) return false;
    value = int(parsed);
    return true;
}
} // namespace

static AppPresentation presentationPolicy() {
    return {true, false, AppPresentation::StatusBar::Hidden};
}
static core::health::RuntimePolicy runtimePolicy() {
    return {true, true, true, isActive()};
}
App app = SCENE_APP_CATALOG_POLICIES("Screen Fit", "safe-area", &kScreen, nullptr, "src/apps/safe_area_app.cpp", kScreens, presentationPolicy, runtimePolicy);
bool isActive() { return NavHandler::instance().top() == &app; }

bool handleCommand(const char* line) {
    if (strncmp(line, "safearea", 8) != 0 || (line[8] && line[8] != ' ' && line[8] != '\t')) return false;
    ensureLoaded();
    char buffer[160];
    if (strlen(line) >= sizeof(buffer)) { Serial.println("[safe-area] ERROR command too long"); return true; }
    strcpy(buffer, line);
    char* words[10];
    int count = 0;
    char* state = nullptr;
    for (char* p = strtok_r(buffer, " \t", &state); p; p = strtok_r(nullptr, " \t", &state)) {
        if (count == 10) { Serial.println("[safe-area] ERROR too many arguments"); return true; }
        words[count++] = p;
    }
    if (count == 1 || (count == 2 && strcmp(words[1], "help") == 0)) { help(); return true; }
    NavHandler& nav = NavHandler::instance();
    const char* op = words[1];
    if (count == 2 && strcmp(op, "on") == 0) {
        if (!nav.root() || !nav.root()->name || strcmp(nav.root()->name, "Boot") == 0 || nav.systemScreenActive() ||
            apps::agent::isRecording() || apps::agent::isSending()) {
            Serial.println("[safe-area] ERROR wait for boot, recording/send, or system alert to finish");
            return true;
        }
        if (!isActive()) {
            nav.popToRoot(); // runs teardown for camera/audio/direct-panel apps
            nav.push(&app);
        }
        s_dirty = true;
    } else if (count == 2 && strcmp(op, "off") == 0) {
        if (isActive()) nav.pop();
    } else if (count == 2 && strcmp(op, "applied") == 0) {
        const auto c = display_safe_area::kProfile;
        Serial.printf("[safe-area] applied left=%d top=%d right=%d bottom=%d radius=%d padding=%d status_bar=%d\n",
            c.left,c.top,c.right,c.bottom,c.radius,c.padding,display_safe_area::kStatusBarHeight);
    } else if (count == 2 && strcmp(op, "status") == 0) {
        // Read only.
    } else if (count == 2 && (strcmp(op, "undo") == 0 || strcmp(op, "redo") == 0)) {
        if (!restoreHistory(strcmp(op, "redo") == 0)) return true;
    } else if (count == 2 && (strcmp(op, "next") == 0 || strcmp(op, "prev") == 0)) {
        s_selected = (s_selected + (strcmp(op, "prev") == 0 ? 5 : 1))%6;
        s_dirty = true;
    } else if (count == 2 && strcmp(op, "reset") == 0) {
        apply(geom::defaults());
    } else if (count == 2 && strcmp(op, "load") == 0) {
        if (!loadSaved()) { Serial.println("[safe-area] ERROR no valid saved profile; values unchanged"); return true; }
    } else if (count == 2 && strcmp(op, "save") == 0) {
        const Config& c = s_config;
        const Record r{1, geom::kWidth, geom::kHeight, uint16_t(c.left), uint16_t(c.top), uint16_t(c.right),
                       uint16_t(c.bottom), uint16_t(c.radius), uint16_t(c.padding)};
        if (!core::store::putBlob("ui_safe_area", "profile_v1", &r, sizeof(r))) {
            Serial.println("[safe-area] ERROR save failed"); return true;
        }
        s_saved = c;
        s_hasSaved = true;
        Serial.println("[safe-area] saved profile; calibration only (app layouts unchanged)");
    } else if ((count == 7 || count == 8) && strcmp(op, "set") == 0) {
        Config next = s_config;
        for (int i = 0; i < count-2; ++i) {
            if (!integer(words[i+2], *field(next, i))) {
                Serial.println("[safe-area] ERROR expected integer pixels; values unchanged"); return true;
            }
        }
        if (!apply(next)) return true;
    } else if (count == 3) {
        const bool selecting = strcmp(op, "select") == 0;
        int index = -1;
        for (int i = 0; i < 6; ++i) if (strcmp(selecting ? words[2] : op, kFields[i]) == 0) index = i;
        if (index < 0) { Serial.println("[safe-area] ERROR unknown field; use safearea help"); return true; }
        if (!selecting) {
            int value = 0;
            if (!integer(words[2], value)) { Serial.println("[safe-area] ERROR expected integer pixels; values unchanged"); return true; }
            Config next = s_config;
            int* target = field(next, index);
            if (words[2][0] == '+' || words[2][0] == '-') *target += value;
            else *target = value;
            if (!apply(next)) return true;
        }
        s_selected = index;
        s_dirty = true;
    } else {
        Serial.println("[safe-area] ERROR syntax; use safearea help"); return true;
    }
    printStatus();
    return true;
}
}} // namespace apps::safe_area
#endif
