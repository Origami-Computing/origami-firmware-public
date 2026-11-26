/**
 * @file photos_app.cpp
 * @brief Photos — browse JPEG photos and AVI videos together on SD card.
 *
 * The photo-library service scans JPEGs in /photos on enter and owns browsing
 * reservations, sync handoff and local-delete/upload ordering. Camera captures
 * are newest first by ID;
 * other files follow by SD modification time/name. Rotary scrolls the gallery.
 *
 * PhotoPreview reduces still images into the canvas. Video thumbnails and playback
 * use the video services. Cover and exit free the gallery decoder workspace.
 *
 * Presentation: media fills the panel (status bar hidden), cover-cropped so
 * there are no bars. ui/photos_view.h draws an index pill at the top, the
 * capture date/time at the bottom (from EXIF; omitted when unknown), a play
 * badge on videos, and on demand an action sheet, a delete
 * confirmation and a brief "Added  to  upload  queue" notice. Overlays repaint in place over the decoded
 * image; only closing an overlay or changing item re-decodes.
 *
 * Input: apps/photos_flow.h owns the control state machine (shared with Kami
 * Screen Studio's interactive Photos demo). This file performs its effects.
 * Turn = previous/next; press = open the action sheet; RB = play the video
 * under the cursor (nothing on a photo); LB = close overlay, else leave Photos.
 * Upload only adds the item to the upload queue; the queue uploads it as usual
 * (browsing pauses uploads, so transfers continue after leaving Photos).
 */

#include "apps/photos_app.h"
#include "apps/videos_app.h"
#include "services/video_thumbnail.h"
#include "apps/photos_flow.h"

#include <Arduino.h>
#include <string.h>
#include "app.h"
#include "services/photo_library.h"
#include "services/uploads.h"
#include "services/gallery_receipt.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "hal/hal_display.h"
#include "hal/hal_photo_preview.h"
#include "hal/hal_input.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/draw.h"
#include "ui/nav_controls.h"
#include "ui/photos_view.h"
#include "ui/photos_loader.h"
#include "apps/photos_cards.h"
#include "core/exif_capture_time.h"
#include "core/board_time.h"
#include <time.h>

namespace apps {
namespace photos {

namespace view = ui::photos_view;
using flow::Mode;
using flow::Command;

static constexpr uint32_t kNoticeMs = 1800;

static flow::Model s_m;                        // mode, index, count, sheet focus
static bool        s_needRender = false;
static bool        s_needPhotoRender = true;   // re-decode media (clears overlays)
static bool        s_overlayFull = false;      // draw scrim + whole overlay, not just focus
static bool        s_offerUpload = true;       // cloud has not confirmed this item (sheet header)
static bool        s_deleteFailed = false;
static const char* s_uploadError=nullptr;
static const char* s_notice = nullptr;         // brief queued confirmation over the photo
static uint32_t    s_noticeUntil = 0;
static char        s_captionDate[20] = "";     // last drawn caption, for status/tests
static const char* s_error = "Cannot read photos";
static services::photos::Library s_library;
static services::video::Thumbnail s_thumbnail;
static int s_thumbnailIndex=-1;
static char s_resumePath[272]{};
static hal::display::PhotoPreview s_preview;
static bool s_lastRendered = false;
static uint32_t s_lastPixels = 0;
static unsigned s_lastNonblack = 0;
static uint32_t s_openStartedMs = 0;
static uint32_t s_openMs = 0;
static uint32_t s_pauseMs = 0;
static uint32_t s_pauseStartedMs = 0;
static bool s_openPending = false;
static bool s_browsePending = false;
static uint32_t s_loaderStart = 0;      // shuffle clock for the loading screen
static uint32_t s_loaderFrameMs = 0;
static bool s_loaderPainted = false;    // full loader on canvas; later frames repaint the stack only
static uint32_t s_loaderFrames = 0;     // frames painted since open (serial evidence)
static bool s_animateWait = true;       // shuffle while uploads pause (dev toggle: photos wait-anim)

static const services::photos::Library::File& current() { return s_library[s_m.index]; }

void dumpStatus(Stream& output) {
    const bool viewing = s_m.viewing() && s_m.count > 0;
    output.printf("[photos] status active=%d ready=%d index=%d count=%d ok=%d pixels=%08lx nonblack=%u menu=%d confirm=%d action=%s rows=%d cloud=%s delete_error=%d date=%s notice=%s open_ms=%lu pause_ms=%lu catalog_us=%lu alloc_us=%lu metadata=%lu path=%s\n",
                  NavHandler::instance().top() == &app ? 1 : 0,
                  viewing && !s_needRender && (!current().video || s_thumbnailIndex==s_m.index) ? 1 : 0,
                  viewing ? s_m.index + 1 : 0, s_m.count,
                  s_lastRendered ? 1 : 0, (unsigned long)s_lastPixels, s_lastNonblack,
                  s_m.overlay() ? 1 : 0, s_m.mode == Mode::Confirm ? 1 : 0,
                  s_m.focusName(), s_m.overlay() ? s_m.sheet.count : 0,
                  s_m.overlay() ? (s_offerUpload ? "no" : "yes") : "-",
                  s_deleteFailed ? 1 : 0, s_captionDate[0] ? s_captionDate : "-", s_noticeUntil && s_notice ? s_notice : "-",
                  (unsigned long)s_openMs, (unsigned long)s_pauseMs,
                  (unsigned long)s_library.timings().totalUs,
                  (unsigned long)s_library.timings().allocationUs,
                  (unsigned long)s_library.timings().metadataReads,
                  viewing ? current().path : "-");
}

static bool pauseBrowsingIo() {
    const auto result = s_library.beginBrowsing(true);
    s_browsePending = result == services::photos::Library::BrowseResult::Pending;
    if (s_browsePending) {s_error="Preparing preview...";return false;}
    if (result == services::photos::Library::BrowseResult::Ready) return true;
    s_error = result == services::photos::Library::BrowseResult::UploadsBusy ? "Uploads busy" : "Network busy";
    return false;
}

static void redrawImage() {
    s_needPhotoRender = true;
    s_needRender = true;
}

static void loadPhotos() {
    s_deleteFailed = false;
    s_uploadError = nullptr;
    s_noticeUntil = 0;
    flow::loadFailed(s_m);
    s_thumbnail.clear();s_thumbnailIndex=-1;
    s_library.clear();
    if (!s_browsePending) s_pauseStartedMs = millis();
    const bool paused = pauseBrowsingIo();
    s_pauseMs = millis() - s_pauseStartedMs;
    if (paused) {
        const auto result = s_library.load();
        switch (result) {
            case services::photos::Library::LoadResult::Ok: {
                const int count = (int)s_library.count();
                int index = 0;
                if (s_resumePath[0])
                    for (int i = 0; i < count; ++i) if (!strcmp(s_resumePath, s_library[i].path)) { index = i; break; }
                flow::loaded(s_m, count, index);
                break;
            }
            case services::photos::Library::LoadResult::NoSd: s_error = "SD card unavailable"; break;
            case services::photos::Library::LoadResult::NoMemory: s_error = "Not enough memory"; break;
            case services::photos::Library::LoadResult::ReadError: s_error = "Cannot read photos"; break;
        }
    }
    if (!s_browsePending) s_resumePath[0] = 0;
    Serial.printf("[photos] catalog count=%d state=%d loader_frames=%lu ms=%lu newest=%s\n", s_m.count, (int)s_m.mode, (unsigned long)s_loaderFrames, (unsigned long)(millis() - s_loaderStart),
                  s_m.count ? s_library[0].path : "-");
    redrawImage();
}

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------

static void deletePhoto() {
    if (!s_library.browsing() || s_m.count <= 0) return;
    char path[sizeof(services::photos::Library::File::path)];
    strlcpy(path, current().path, sizeof(path));
    int removedUploads = 0;
    if (!s_library.removeAt((size_t)s_m.index, &removedUploads)) {
        Serial.printf("[photos] delete ok=0 path=%s\n", path);
        s_deleteFailed = true;
        flow::deleteFailed(s_m);
        s_overlayFull = true;
        redrawImage();
        return;
    }
    s_thumbnail.clear();s_thumbnailIndex=-1;
    flow::removed(s_m, (int)s_library.count());
    s_lastRendered = false;
    s_deleteFailed = false;
    redrawImage();
    Serial.printf("[photos] delete ok=1 queue=%d count=%d path=%s\n", removedUploads, s_m.count, path);
}

static void uploadFailed(const char* message) {
    s_uploadError = message;
    flow::uploadFailed(s_m);   // header shows the error
    s_overlayFull = true;
    redrawImage();
}

// Add the selected item to the upload queue and keep browsing. The queue's
// worker uploads it as usual once the gallery's browsing pause ends.
static void uploadSelected() {
    using E=UploadQueue::VideoEnqueue;
    if(!s_library.browsing() || s_m.index<0 || s_m.index>=s_m.count)return;
    if(!services::uploads::ensureInit()){ uploadFailed("Upload unavailable"); return; }
    auto queued = [](const char* message) {
        s_uploadError = nullptr;
        flow::uploadQueued(s_m);
        s_notice = message;
        s_noticeUntil = millis() + kNoticeMs;
        if (!s_noticeUntil) s_noticeUntil = 1;
        redrawImage();
    };
    if(!current().video){
        auto& queue=services::uploads::queue();
        const auto& file=current();
        const char* name=strrchr(file.path,'/');
        const auto view=queue.view(name?name+1:file.path);
        if(view.count && !strcasecmp(view.item.filePath,file.path)){
            if(view.item.status==QSTATUS_FAILED && queue.forceSend(view.item.id)!=UploadQueue::ForceResult::Scheduled){
                uploadFailed("Queue busy, try again"); return;
            }
            queued(view.item.status==QSTATUS_FAILED ? "Added  to  upload  queue" : "Already  in  upload  queue"); return;
        }
        if(queue.enqueue("image",file.path,"image/jpeg",false)){ queued("Added  to  upload  queue"); return; }
        uploadFailed("Cannot queue photo"); return;
    }
    switch(services::uploads::queue().enqueueVideo(current().path)){
      case E::Queued: queued("Added  to  upload  queue"); return;
      case E::AlreadyQueued: queued("Already  in  upload  queue"); return;
      case E::Busy: uploadFailed("Upload busy, try again"); return;
      case E::Full: uploadFailed("Upload queue full"); return;
      case E::AccountRequired: uploadFailed("Connect account first"); return;
      case E::AccountChanged: uploadFailed("Queued for other account"); return;
      case E::Invalid: uploadFailed("Cannot upload video"); return;
      case E::StorageUnavailable: uploadFailed("Cannot save request"); return;
    }
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

static void drawSheet(TFT_eSprite& canvas, view::Bounds s, bool full) {
    view::SheetRow rows[3];
    for (int i = 0; i < s_m.sheet.count; ++i) {
        const auto action = s_m.sheet.at(i);
        rows[i] = action == policy::Action::Play   ? view::SheetRow{"Play", view::Icon::Play, false}
                : action == policy::Action::Upload ? view::SheetRow{"Upload", view::Icon::Upload, false}
                                                   : view::SheetRow{"Delete", view::Icon::Trash, true};
    }
    const view::SheetModel model{current().video ? "Video" : "Photo", s_m.index + 1, s_m.count, !s_offerUpload,
                                 s_deleteFailed ? "Cannot delete" : s_uploadError,
                                 rows, s_m.sheet.count, s_m.row};
    view::sheet(canvas, s, model, full);
}

static void drawOverlay(TFT_eSprite& canvas, view::Bounds s, bool full) {
    if (s_m.mode == Mode::Sheet) drawSheet(canvas, s, full);
    else if (s_m.mode == Mode::Confirm) view::confirmDelete(canvas, s, current().video, s_m.confirmDelete, full);
}

// Loading screen: shuffling cards of the newest photos (or placeholders).
static void paintLoader(TFT_eSprite& canvas, bool full) {
    const view::Bounds s{(int)canvas.width(), (int)canvas.height()};
    const auto deck = cards::deck();
    const uint32_t ms = millis() - s_loaderStart;
    if (full || !s_loaderPainted) {
        ui::photos_loader::photosLoader(canvas, s, deck, ms);
        hal::display::markAllDirty();
    } else {
        namespace pl = ui::photos_loader;
        pl::stack(canvas, s, deck, ms);
        hal::display::markDirtyRect(pl::kStackX, pl::kStackY, pl::kStackW, pl::kStackH);
    }
    s_loaderPainted = true;
    ++s_loaderFrames;
    s_loaderFrameMs = millis();
}


// First loader frame before the synchronous catalog read (replaces the old splash).
static void showLoaderNow() {
    if (hal::display::isOptimizedRender()) return;
    cards::load();
    s_loaderStart = millis();
    s_loaderFrames = 0;
    s_loaderPainted = false;
    paintLoader(hal::display::canvas(), true);
    hal::display::flush();
}

// Formats the photo's EXIF capture time as local "24 Nov" / "4:12 pm". Times
// with an offset (Origami writes UTC +00:00) convert through the device
// timezone; times without one are already local. Leaves both empty when the
// photo carries no capture time, so nothing is drawn.
static void captureCaption(const uint8_t* jpeg, size_t bytes, char* date, char* clock) {
    core::exif::CaptureTime t{};
    if (!core::exif::readCaptureTime(jpeg, bytes, t)) return;
    int year = t.year, month = t.month, day = t.day, hour = t.hour, minute = t.minute;
    if (t.hasOffset) {
        const time_t utc = (time_t)core::exif::toUnix(t);
        struct tm local{};
        if (!localtime_r(&utc, &local)) return;
        year = local.tm_year + 1900; month = local.tm_mon + 1; day = local.tm_mday;
        hour = local.tm_hour; minute = local.tm_min;
    }
    int currentYear = year;  // without a trusted clock, never claim a different year
    if (core::time::isSynced()) {
        const time_t now = ::time(nullptr);
        struct tm today{};
        if (localtime_r(&now, &today)) currentYear = today.tm_year + 1900;
    }
    const auto text = view::formatCapture(year, month, day, hour, minute, currentYear);
    strlcpy(date, text.date, 20);
    strlcpy(clock, text.time, 12);
}

static void onDraw(TFT_eSprite& canvas) {
    if (!s_needRender) return;
    s_needRender = false;

    const int16_t W  = (int16_t)canvas.width();
    const int16_t H  = (int16_t)canvas.height();
    const view::Bounds s{W, H};

    if (s_m.mode == Mode::Error && s_browsePending) {
        // Waiting for uploads to pause: animate instead of showing an error.
        if (s_loaderPainted && (!s_animateWait || millis() - s_loaderFrameMs < ui::photos_loader::kFrameMs)) return;
        paintLoader(canvas, false);
        return;
    }
    // On open, keep the cards up until the first item's thumbnail is ready
    // instead of flashing a black "Loading preview..." frame.
    if (s_loaderPainted && s_m.viewing() && s_m.count && current().video && s_thumbnailIndex != s_m.index) {
        s_needRender = true;   // the loader already on screen stays until then
        return;
    }
    s_loaderPainted = false;

    if (s_m.mode == Mode::Error || s_m.mode == Mode::Empty) {
        if (s_m.mode == Mode::Error) view::message(canvas, s, s_error, "Press the dial to try again.", nullptr);
        else view::message(canvas, s, "No photos yet", "Photos and videos from Camera", "will show up here.");
        hal::display::markAllDirty();
        return;
    }

    // In-place updates over the decoded image: no SD/JPEG work.
    if (!s_needPhotoRender) {
        drawOverlay(canvas, s, s_overlayFull);
        s_overlayFull = false;
        hal::display::markAllDirty();
        return;
    }
    s_needPhotoRender = false;

    // Media fills the panel: cover scaling crops the overflow, so no bars.
    canvas.fillRect(0, 0, W, H, view::kBlack);
    const char* path = current().path;
    const bool video = current().video;
    const uint32_t decodeStarted = millis();
    s_preview.setCover(true);  // photos and video thumbnails fill the panel (cover crop)
    const bool rendered = video
        ? s_thumbnailIndex==s_m.index && (s_preview.drawBuffer(s_thumbnail.data(),s_thumbnail.size(),0,0,W,H) ||
                                          hal::display::drawJpegBufferScaled(s_thumbnail.data(),s_thumbnail.size(),0,0,W,H,
                                                                             hal::display::ImageFit::Cover))
        : s_preview.draw(path,0,0,W,H);
    const uint32_t decodeMs = millis() - decodeStarted;

    // A sparse canvas fingerprint makes blank/stale gallery renders testable over
    // serial. Taken before any chrome so it describes the decoded media only.
    uint32_t pixels = 2166136261u;
    unsigned nonblack = 0;
    for (int y = 0; y < H; y += 8) {
        for (int x = 0; x < W; x += 8) {
            const uint16_t pixel = canvas.readPixel(x, y);
            pixels = (pixels ^ pixel) * 16777619u;
            if (pixel) ++nonblack;
        }
    }
    Serial.printf("[photos] render index=%d count=%d ok=%d decode_ms=%lu pixels=%08lx nonblack=%u modified=%llu path=%s\n",
                  s_m.index + 1, s_m.count, rendered ? 1 : 0, (unsigned long)decodeMs, (unsigned long)pixels, nonblack,
                  (unsigned long long)current().modified, path);
    s_lastRendered = rendered;
    s_lastPixels = pixels;
    s_lastNonblack = nonblack;
    if (s_openPending && (!video || s_thumbnailIndex==s_m.index)) {
        s_openMs = millis() - s_openStartedMs;
        s_openPending = false;
    }

    if (!rendered) {
        view::textCenter(canvas, s, video && s_thumbnailIndex!=s_m.index ? "Loading preview..." : "Cannot open this file",
                         W / 2, H / 2 - view::capHeight(18) / 2, 18, view::kMuted);
    }
    // Loader cards sample the media before any chrome; cover fills all rows.
    if (rendered) cards::capture(canvas, s_m.index, path, current().modified, 0, H);
    const bool notice = s_noticeUntil && s_notice && s_m.mode == Mode::Browse;
    if (video && rendered) view::playBadge(canvas, s, W / 2, H / 2);
    view::indexPill(canvas, s, s_m.index + 1, s_m.count);

    // Bottom caption: capture date and time (photos with EXIF), plus the
    // duration for videos. The queued notice takes this spot while shown.
    char date[20] = "", clock[12] = "", duration[12] = "";
    if (!video && rendered && s_preview.jpeg()) captureCaption(s_preview.jpeg(), s_preview.jpegBytes(), date, clock);
    if (video && rendered) {
        const uint32_t seconds = (s_thumbnail.durationMs() + 999) / 1000;
        snprintf(duration, sizeof duration, "%lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
    }
    strlcpy(s_captionDate, date, sizeof s_captionDate);
    if (notice) view::notice(canvas, s, s_notice, true);
    else {
        const char* parts[] = {date, clock, duration};
        view::dateCaption(canvas, s, parts, 3);
    }
    if (s_m.overlay()) drawOverlay(canvas, s, true);
    s_overlayFull = false;

    hal::display::markAllDirty();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void activateGallery() {
    s_resumePath[0]=0;
    s_openStartedMs = millis();
    s_openPending = true;
    showLoaderNow();
    loadPhotos();
}

static void saveCards() {
    if (!s_library.browsing()) return;
    cards::Key keys[cards::kMax];
    int n = 0;
    for (; n < cards::kMax && n < (int)s_library.count(); ++n) keys[n] = {s_library[n].path, s_library[n].modified};
    cards::keepAndSave(keys, n);
}

// Saving is skipped when only covered (playing a video) so playback starts promptly.
static void closeGallery(bool save) {
    if (save) saveCards();
    s_loaderPainted = false;
    s_preview.close();
    s_browsePending = false;
    s_noticeUntil = 0;
    s_thumbnail.clear();s_thumbnailIndex=-1;
    s_library.close();
    flow::closed(s_m);
}
static void exitApp() { closeGallery(true); }

static void coverGallery(){
    if(s_m.count && s_m.viewing())strlcpy(s_resumePath,current().path,sizeof(s_resumePath));
    closeGallery(false);
}
static void resumeGallery(){showLoaderNow();loadPhotos();}

// ---------------------------------------------------------------------------
// Tick / Input
// ---------------------------------------------------------------------------

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void setWaitAnimationForDevelopment(bool on) { s_animateWait = on; }
bool previewLoaderForDevelopment(bool video, uint32_t ms) {
    if (NavHandler::instance().top() != &app || !s_m.viewing() || !s_m.count) return false;
    TFT_eSprite& canvas = hal::display::canvas();
    const view::Bounds s{(int)canvas.width(), (int)canvas.height()};
    if (!video) {
        ui::photos_loader::photosLoader(canvas, s, cards::deck(), ms);
    } else {
        if (!current().video || s_thumbnailIndex != s_m.index) return false;
        canvas.fillRect(0, 0, s.w, s.h, view::kBlack);
        if (!s_preview.drawBuffer(s_thumbnail.data(), s_thumbnail.size(), 0, 0, s.w, s.h))
            hal::display::drawJpegBufferScaled(s_thumbnail.data(), s_thumbnail.size(), 0, 0, s.w, s.h,
                                               hal::display::ImageFit::Cover);
        char name[40];
        const char* base = strrchr(current().path, '/');
        strlcpy(name, base ? base + 1 : current().path, sizeof(name));
        if (char* dot = strrchr(name, '.')) *dot = 0;
        char duration[16];
        const uint32_t sec = (s_thumbnail.durationMs() + 999) / 1000;
        snprintf(duration, sizeof duration, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        ui::photos_loader::videoLoader(canvas, s, name, duration, ms);
    }
    hal::display::markAllDirty();
    return true;
}
#endif

static void onTick(uint32_t now) {
    if (s_browsePending) { loadPhotos(); return; }
    if(s_m.viewing() && s_thumbnailIndex!=s_m.index){
        s_thumbnail.clear();
        const uint32_t started=millis();
        if(current().video)s_thumbnail.load(current().path);
        if(current().video)Serial.printf("[photos] thumbnail index=%d ms=%lu\n",s_m.index+1,(unsigned long)(millis()-started));
        s_thumbnailIndex=s_m.index;
        redrawImage();
    }
    if (s_noticeUntil && (int32_t)(now - s_noticeUntil) >= 0) {
        s_noticeUntil = 0;
        if (s_m.mode == Mode::Browse) redrawImage();   // clears the notice
    }
}

static void onInput(hal::input::InputEvent ev) {
    using ET = hal::input::EventType;
    flow::Input in;
    if (ui::nav::isBackTap(ev)) in = flow::Input::Back;
    else if (ev.type == ET::ButtonB_Tap) in = flow::Input::Right;
    else if (ui::nav::isEnterPress(ev)) in = flow::Input::Press;
    else if (ev.type == ET::Scroll && ev.delta != 0) in = flow::Input::Turn;
    else return;

    // While the library handoff is pending only leaving is allowed.
    if (s_browsePending && !s_m.overlay() && in != flow::Input::Back) return;

    flow::Item item{false, false};
    if (s_m.viewing() && s_m.count) {
        item.video = current().video;
        // The receipt lookup is only needed when a sheet may open, not per dial tick.
        const bool mayOpenSheet = s_m.mode == Mode::Browse && in == flow::Input::Press;
        item.uploaded = mayOpenSheet && services::gallery_receipt::confirmed(current().path);
    }
    switch (flow::apply(s_m, in, ev.delta, item, !s_needRender)) {
        case Command::None: break;
        case Command::Repaint: s_needRender = true; break;
        case Command::RedrawImage:
            s_uploadError = s_m.mode == Mode::Sheet ? s_uploadError : nullptr;
            s_deleteFailed = s_m.mode == Mode::Sheet && s_deleteFailed;
            s_overlayFull = true;
            redrawImage();
            break;
        case Command::OpenSheet:
            s_uploadError = nullptr;
            s_deleteFailed = false;
            s_offerUpload = !item.uploaded;
            s_overlayFull = true;  // scrim over the image already on the canvas
            s_needRender = true;
            break;
        case Command::Browse:
            s_noticeUntil = 0;
            redrawImage();
            break;
        case Command::Play:
            // Hand over the thumbnail already in memory so the player can show it while it starts.
            if (s_thumbnailIndex == s_m.index && s_thumbnail.size())
                (void)apps::videos::openFile(current().path, s_thumbnail.data(), s_thumbnail.size(), s_thumbnail.durationMs());
            else
                (void)apps::videos::openFile(current().path);
            break;
        case Command::Upload: uploadSelected(); break;
        case Command::Delete: deletePhoto(); break;
        case Command::Exit: NavHandler::instance().pop(); break;
        case Command::Reload: loadPhotos(); break;
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
    s_overlayFull = true;
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
static core::health::RuntimePolicy runtimePolicy() {
    core::health::RuntimePolicy policy{};
    policy.reportActivity = s_browsePending;
    policy.allowIdleReturn = true;
    return policy;
}
// Media owns the whole panel; the gallery draws no system status strip.
static AppPresentation presentation() {
    return AppPresentation{true, false, AppPresentation::StatusBar::Hidden};
}
App app = {"Photos",&kScreenPhotos,nullptr,nullptr,exitApp,"photos","src/apps/photos_app.cpp",
    kScreens,uint8_t(sizeof(kScreens)/sizeof(kScreens[0])),presentation,runtimePolicy,
    activateGallery,resumeGallery,nullptr,coverGallery};

}  // namespace photos
}  // namespace apps
