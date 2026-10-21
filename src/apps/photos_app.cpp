/**
 * @file photos_app.cpp
 * @brief Photos — browse JPEG images saved to /photos on SD card.
 *
 * The photo-library service scans JPEGs in /photos on enter and owns browsing
 * reservations, sync handoff and local-delete/upload ordering. Camera captures
 * are newest first by ID;
 * other files follow by SD modification time/name. Rotary scrolls the gallery.
 *
 * All JPEG decoding and scaling is handled by hal::display::drawJpegFileScaled(),
 * which writes into the canvas sprite via the normal canvas pipeline.
 * All UI chrome uses ui::scene::Builder — no direct TFT calls in this file.
 */

#include "apps/photos_app.h"
#include "apps/photos_policy.h"

#include <Arduino.h>
#include <string.h>
#include "app.h"
#include "services/photo_library.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/draw.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"

namespace apps {
namespace photos {

enum PhotosState { PH_EMPTY, PH_VIEWING, PH_SYNCING, PH_ERROR };

static PhotosState s_state      = PH_EMPTY;
static int         s_count      = 0;
static int         s_idx        = 0;   // 0 = newest photo
static bool        s_needRender = false;
static bool        s_needPhotoRender = true;
static bool        s_actionsOpen = false;
static policy::Action s_action = policy::Action::Escape;
static bool        s_deleteFailed = false;
static const char* s_error = "Cannot read photos";
static services::photos::Library s_library;
static bool s_lastRendered = false;
static uint32_t s_lastPixels = 0;
static unsigned s_lastNonblack = 0;
static uint32_t s_openStartedMs = 0;
static uint32_t s_openMs = 0;
static uint32_t s_pauseMs = 0;
static bool s_openPending = false;

void dumpStatus(Stream& output) {
    const bool viewing = s_state == PH_VIEWING && s_count > 0;
    output.printf("[photos] status active=%d ready=%d index=%d count=%d ok=%d pixels=%08lx nonblack=%u menu=%d action=%s delete_error=%d open_ms=%lu pause_ms=%lu catalog_us=%lu alloc_us=%lu metadata=%lu path=%s\n",
                  NavHandler::instance().top() == &app ? 1 : 0,
                  viewing && !s_needRender ? 1 : 0, viewing ? s_idx + 1 : 0, s_count,
                  s_lastRendered ? 1 : 0, (unsigned long)s_lastPixels, s_lastNonblack,
                  s_actionsOpen ? 1 : 0, s_action == policy::Action::Delete ? "delete" : "esc",
                  s_deleteFailed ? 1 : 0,
                  (unsigned long)s_openMs, (unsigned long)s_pauseMs,
                  (unsigned long)s_library.timings().totalUs,
                  (unsigned long)s_library.timings().allocationUs,
                  (unsigned long)s_library.timings().metadataReads,
                  viewing ? s_library[s_idx].path : "-");
}

// Sync (B): drain the upload queue to the cloud with a progress bar.
static int      s_syncTotal   = 0;   // pending items at the moment sync started
static uint32_t s_syncDoneMs  = 0;   // millis() when the queue first hit empty (0 = not done)
static uint32_t s_syncPollMs  = 0;   // redraw rate-limit

static bool pauseBrowsingIo() {
    const auto result = s_library.beginBrowsing();
    if (result == services::photos::Library::BrowseResult::Ready) return true;
    s_error = result == services::photos::Library::BrowseResult::UploadsBusy ? "Uploads busy" : "Network busy";
    return false;
}

static void loadPhotos() {
    s_actionsOpen = false;
    s_deleteFailed = false;
    s_needPhotoRender = true;
    s_idx = 0;
    s_count = 0;
    s_state = PH_ERROR;
    s_library.clear();
    const uint32_t pauseStarted = millis();
    const bool paused = pauseBrowsingIo();
    s_pauseMs = millis() - pauseStarted;
    if (paused) {
        const auto result = s_library.load();
        switch (result) {
            case services::photos::Library::LoadResult::Ok:
                s_count = (int)s_library.count();
                s_state = s_count ? PH_VIEWING : PH_EMPTY;
                break;
            case services::photos::Library::LoadResult::NoSd: s_error = "SD card unavailable"; break;
            case services::photos::Library::LoadResult::NoMemory: s_error = "Not enough memory"; break;
            case services::photos::Library::LoadResult::ReadError: s_error = "Cannot read photos"; break;
        }
    }
    Serial.printf("[photos] catalog count=%d state=%d newest=%s\n", s_count, (int)s_state,
                  s_count ? s_library[0].path : "-");
    s_needRender = true;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

static void closeActions() {
    s_actionsOpen = false;
    s_deleteFailed = false;
    s_needPhotoRender = true;
    s_needRender = true;
}

static void deletePhoto() {
    if (!s_library.browsing() || s_count <= 0) return;
    char path[sizeof(services::photos::Library::File::path)];
    strlcpy(path, s_library[s_idx].path, sizeof(path));
    int removedUploads = 0;
    if (!s_library.removeAt((size_t)s_idx, &removedUploads)) {
        Serial.printf("[photos] delete ok=0 path=%s\n", path);
        s_deleteFailed = true;
        s_action = policy::Action::Escape;
        s_needRender = true;
        return;
    }
    s_count = (int)s_library.count();
    s_idx = policy::indexAfterRemoval(s_idx, s_count);
    s_state = s_count ? PH_VIEWING : PH_EMPTY;
    s_lastRendered = false;
    closeActions();
    Serial.printf("[photos] delete ok=1 queue=%d count=%d path=%s\n", removedUploads, s_count, path);
}

static void drawActions(ui::scene::Scene& scene, int16_t W, int16_t H) {
    const int16_t width = 122;
    const int16_t height = 32;
    const int16_t gap = 6;
    const int16_t x = W - kScreenInsetX - width;
    const int16_t top = kSystemStatusBarH + (H - kSystemStatusBarH - 22 - height * 2 - gap) / 2;
    for (int row = 0; row < 2; ++row) {
        const bool selected = (row == 0) == (s_action == policy::Action::Delete);
        const uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
        const uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
        const int16_t y = top + row * (height + gap);
        const char* id = row == 0 ? "photos.action.delete" : "photos.action.esc";
        auto* border = scene.addRect(id, {x, y, width, height}, bg, 6, 2);
        if (border) {
            border->strokeColor = selected ? TFT_WHITE : TFT_BLACK;
            border->strokeWidth = 1;
        }
        if (selected) {
            // An explicit cursor avoids relying on black/white inversion to
            // communicate which of these destructive/non-destructive rows is active.
            scene.addLine("photos.focus.top", x + 6, y + 9, x + 13, y + 16, fg, 3);
            scene.addLine("photos.focus.bottom", x + 6, y + 23, x + 13, y + 16, fg, 3);
            scene.addLine("photos.focus.top.bold", x + 5, y + 9, x + 12, y + 16, fg, 3);
            scene.addLine("photos.focus.bottom.bold", x + 5, y + 23, x + 12, y + 16, fg, 3);
        }
        scene.addLabel(row == 0 ? "photos.action.delete.label" : "photos.action.esc.label",
                       {(int16_t)(x + 18), y, 76, height}, row == 0 ? "Delete" : "Esc",
                       fg, bg, 2, ui::scene::TextAlign::MiddleCenter, 3);
        const int16_t ix = x + width - 23;
        const int16_t iy = y + 8;
        if (row == 0) {
            auto* body = scene.addRect("photos.trash.body", {(int16_t)(ix + 2), (int16_t)(iy + 5), 12, 13}, bg, 0, 3);
            if (body) { body->strokeColor = fg; body->strokeWidth = 1; }
            scene.addLine("photos.trash.lid", ix, iy + 3, ix + 16, iy + 3, fg, 3);
            scene.addLine("photos.trash.handle", ix + 5, iy, ix + 11, iy, fg, 3);
            scene.addLine("photos.trash.left", ix + 6, iy + 8, ix + 6, iy + 14, fg, 3);
            scene.addLine("photos.trash.right", ix + 10, iy + 8, ix + 10, iy + 14, fg, 3);
        } else {
            scene.addLine("photos.escape.shaft", ix, iy + 8, ix + 16, iy + 8, fg, 3);
            scene.addLine("photos.escape.top", ix + 10, iy + 2, ix + 16, iy + 8, fg, 3);
            scene.addLine("photos.escape.bottom", ix + 10, iy + 14, ix + 16, iy + 8, fg, 3);
        }
    }
    if (s_deleteFailed) {
        scene.addRect("photos.delete.error.bg", {0, (int16_t)(H - 44), W, 22}, TFT_WHITE, 0, 4);
        scene.addLabel("photos.delete.error", {0, (int16_t)(H - 44), W, 22}, "Cannot delete photo",
                       TFT_BLACK, TFT_WHITE, 1, ui::scene::TextAlign::MiddleCenter, 5);
    }
}

static void onDraw(TFT_eSprite& canvas) {
    if (!s_needRender) return;
    s_needRender = false;

    const int16_t W  = (int16_t)canvas.width();
    const int16_t H  = (int16_t)canvas.height();
    const int16_t cH = H - kSystemStatusBarH;

    ui::scene::Scene& scene = ui::scene::scratchScene();
    scene.clear();
    ui::scene::setCurrentScene(&scene);
    ui::scene::Builder b(scene, W, H);

    if (s_state == PH_SYNCING) {
        const auto sync = s_library.syncStatus();
        const int pend = sync.pending;
        const int failed = sync.failed;
        int done = s_syncTotal - pend;
        if (done < 0) done = 0;
        if (done > s_syncTotal) done = s_syncTotal;
        const bool finished = (pend == 0);

        b.clearContent(kUiBg);
        b.panelHeader("sync.title", "Sync to cloud");
        char line[40];
        if (s_syncTotal == 0) {
            b.labelCenterMuted("sync.msg", "Nothing to sync");
        } else if (!sync.connected && !finished) {
            b.statusLine("sync.msg", "Connecting Wi-Fi...", kUiMuted);
        } else if (finished) {
            if (failed > 0) snprintf(line, sizeof(line), "Synced %d, %d failed", done, failed);
            else            snprintf(line, sizeof(line), "All synced (%d)", s_syncTotal);
            b.statusLine("sync.msg", line, kUiFg);
        } else {
            snprintf(line, sizeof(line), "Uploading %d / %d", done, s_syncTotal);
            b.statusLine("sync.msg", line, kUiFg);
        }
        b.progressBar("sync.bar", (uint32_t)done, (uint32_t)(s_syncTotal > 0 ? s_syncTotal : 1));

        scene.render(canvas);
        hal::display::markAllDirty();
        return;
    }

    if (s_state == PH_ERROR || s_state == PH_EMPTY) {
        b.clearContent(kUiBg);
        if (s_state == PH_ERROR) b.statusLine("photos.error", s_error, kUiFg);
        else b.emptyState("photos.empty", "No photos yet.", "");
        scene.render(canvas);
        hal::display::markAllDirty();
        return;
    }

    // Menu focus changes reuse the existing image on the canvas.
    if (s_actionsOpen && !s_needPhotoRender) {
        drawActions(scene, W, H);
        scene.render(canvas);
        hal::display::markAllDirty();
        return;
    }
    s_needPhotoRender = false;

    // Shared page background around the photo.
    b.clearContent(kUiBg);
    scene.render(canvas);
    scene.clear();

    // Decode and scale JPEG into canvas — all low-level work in the HAL
    const char* path = s_library[s_idx].path;
    const bool rendered = hal::display::drawJpegFileScaled(path, 0, kSystemStatusBarH, W, cH - 22);
    if (!rendered) b.statusLine("photos.read_error", "Cannot open photo", kUiFg);

    // A sparse canvas fingerprint makes blank/stale gallery renders testable over serial.
    uint32_t pixels = 2166136261u;
    unsigned nonblack = 0;
    for (int y = kSystemStatusBarH; y < H - 22; y += 8) {
        for (int x = 0; x < W; x += 8) {
            const uint16_t pixel = canvas.readPixel(x, y);
            pixels = (pixels ^ pixel) * 16777619u;
            if (pixel) ++nonblack;
        }
    }
    Serial.printf("[photos] render index=%d count=%d ok=%d pixels=%08lx nonblack=%u modified=%llu path=%s\n",
                  s_idx + 1, s_count, rendered ? 1 : 0, (unsigned long)pixels, nonblack,
                  (unsigned long long)s_library[s_idx].modified, path);
    s_lastRendered = rendered;
    s_lastPixels = pixels;
    s_lastNonblack = nonblack;
    if (s_openPending) {
        s_openMs = millis() - s_openStartedMs;
        s_openPending = false;
    }

    // Overlay: photo index counter at the bottom
    char indexBuf[24];
    snprintf(indexBuf, sizeof(indexBuf), "%d / %d", s_idx + 1, s_count);
    char footer[64];
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    int nameChars = (W - 16) / 6 - (int)strlen(indexBuf) - 2;
    if (nameChars < 0) nameChars = 0;
    snprintf(footer, sizeof(footer), "%.*s  %s", nameChars, name, indexBuf);
    ui::scene::Builder b2(scene, W, H);
    scene.addRect("photos.footer.bg", {0, (int16_t)(H - 24), (int16_t)W, 24}, kUiBg);
    scene.addLabel("photos.index", {14, (int16_t)(H - 22), (int16_t)(W - 28), 20},
                   footer, kUiFg, kUiBg, 1, ui::scene::TextAlign::MiddleCenter, 2);
    if (s_actionsOpen) drawActions(scene, W, H);
    scene.render(canvas);

    hal::display::markAllDirty();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void activateGallery() {
    s_openStartedMs = millis();
    s_openPending = true;
    ui::showAppOpeningSplash("Photos");
    loadPhotos();
}

static void exitApp() {
    s_actionsOpen = false;
    s_library.close();
    s_count = 0;
}

// ---------------------------------------------------------------------------
// Tick / Input
// ---------------------------------------------------------------------------

// Begin draining the upload queue to the cloud. Photos stopped WiFi for memory,
// so bring it back; the background upload worker does the actual transfers once
// connected and unpaused.
static void startSync() {
    s_syncTotal = s_library.beginSync();
    s_syncDoneMs = 0;
    s_syncPollMs = 0;
    s_state      = PH_SYNCING;
    s_needRender = true;
}

// Reacquire the browsing I/O pause before shutting WiFi down again.
static void endSync() {
    s_syncDoneMs = 0;
    s_state = pauseBrowsingIo() ? (s_count > 0 ? PH_VIEWING : PH_EMPTY) : PH_ERROR;
    s_needRender = true;
    s_needPhotoRender = true;
}

static void onTick(uint32_t now) {
    if (s_state != PH_SYNCING) return;
    const int pending = s_library.syncStatus().pending;

    if (pending == 0) {
        if (s_syncDoneMs == 0) s_syncDoneMs = now;
        // Hold the "All synced" result briefly, then return to browsing.
        if ((uint32_t)(now - s_syncDoneMs) > 2500) { endSync(); return; }
    } else {
        s_syncDoneMs = 0;
    }

    if ((uint32_t)(now - s_syncPollMs) >= 300) {
        s_syncPollMs = now;
        s_needRender = true;   // advance the progress bar
    }
}

static void onInput(hal::input::InputEvent ev) {
    using ET = hal::input::EventType;

    if (s_actionsOpen) {
        if (ui::nav::isBackTap(ev)) closeActions();
        else if (ev.type == ET::Scroll) {
            const auto next = policy::moveAction(s_action, ev.delta);
            if (next != s_action) { s_action = next; s_needRender = true; }
        } else if (ui::nav::isEnterPress(ev) && !s_needRender) {
            if (s_action == policy::Action::Escape) closeActions();
            else deletePhoto();
        }
        return;  // RB must not start cloud sync while choosing a destructive action.
    }

    if (ui::nav::isBackTap(ev)) {
        if(s_state==PH_SYNCING) { endSync(); return; }
        NavHandler::instance().pop();
        return;
    }
    if (ui::nav::isEnterPress(ev) && (s_state == PH_ERROR || s_state == PH_EMPTY)) {
        loadPhotos();
        return;
    }
    if (ev.type == ET::ButtonB_Tap) {      // B = sync to cloud
        if (s_state == PH_SYNCING) { endSync(); return; }  // B again = stop watching
        startSync();
        return;
    }

    if (s_state != PH_VIEWING) return;     // ignore scroll while syncing / empty

    if (ui::nav::isEnterPress(ev) && !s_needRender) {
        s_actionsOpen = true;
        s_action = policy::Action::Escape;
        s_deleteFailed = false;
        s_needRender = true;
        return;
    }

    if (ev.type == ET::Scroll && ev.delta != 0) {
        const int prev = s_idx;
        s_idx += ev.delta;
        if (s_idx < 0)        s_idx = 0;
        if (s_idx >= s_count) s_idx = s_count - 1;
        if (s_idx != prev) {
            s_needRender = true;
            s_needPhotoRender = true;
        }
    }
}

static bool onChord() { return false; }

// ---------------------------------------------------------------------------
// Screen + App descriptor
// ---------------------------------------------------------------------------

// View refresh only. Catalog/network operations belong to app activation/resume.
static void enterView() {
    s_needRender = true;
    s_needPhotoRender = true;
}

const ::scene::Screen kScreenPhotos = {
    "Photos",
    enterView,
    nullptr,
    onTick,
    onInput,
    onChord,
    onDraw,
    "photos",
    nullptr,
};

static const ::scene::Screen* const kScreens[] = { &kScreenPhotos };
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Photos", "photos", &kScreenPhotos,
                            activateGallery, activateGallery, exitApp,
                            "src/apps/photos_app.cpp", kScreens, nullptr, nullptr);

}  // namespace photos
}  // namespace apps
