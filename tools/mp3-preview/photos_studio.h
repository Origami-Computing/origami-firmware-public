#pragma once
// Photos gallery pages for Kami Screen Studio (pages 300-305).
// The proposed screens are drawn by include/ui/photos_view.h, the same code
// src/apps/photos_app.cpp runs on the device; page 305 reproduces the earlier
// staging layout for comparison. Sample images are procedural placeholders.
#include "photos_study.h"

namespace photos_studio {

constexpr unsigned kFirstPage = 300, kLastPage = 311;
inline bool handles(unsigned page) { return page >= kFirstPage && page <= kLastPage; }

// Stream the raster as horizontal runs, the renderer's command format.
template <class C> void emit(C& out, const photos_study::Raster& r) {
    using photos_study::W;
    using photos_study::H;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W;) {
            const uint16_t ink = r.px[y * W + x];
            int end = x + 1;
            while (end < W && r.px[y * W + end] == ink) ++end;
            out.fillRect(x, y, end - x, 1, ink);
            x = end;
        }
}

inline void draw(photos_study::Raster& r, unsigned page, unsigned sel) {
    using namespace photos_study;
    using pv::Icon;
    const Media photo{Sample::Sunset, false, "", false};
    const Media uploaded{Sample::Lake, false, "", true};
    const Media video{Sample::Street, true, "0:12", false};
    const pv::SheetRow photoRows[] = {{"Upload", Icon::Upload, false}, {"Delete", Icon::Trash, true}};
    const pv::SheetRow videoRows[] = {{"Play", Icon::Play, false}, {"Upload", Icon::Upload, false}, {"Delete", Icon::Trash, true}};
    switch (page) {
        case 300:  // Browse
            if (sel == 2) proposedBrowse(r, video, false, 4, 12);
            else proposedBrowse(r, photo, sel == 1, 3, 12);
            break;
        case 301:  // Action sheet
            if (sel <= 1) proposedSheet(r, photo, photoRows, 2, sel, "Photo", 3, 12);
            else if (sel <= 4) proposedSheet(r, video, videoRows, 3, sel - 2, "Video", 4, 12);
            else if (sel == 5) proposedSheet(r, uploaded, photoRows, 2, 0, "Photo", 5, 12);  // re-upload
            else proposedSheet(r, photo, photoRows, 2, 0, "Photo", 3, 12, "Upload queue full");
            break;
        case 302:  // Delete confirmation
            proposedConfirm(r, sel == 2 ? video : photo, sel == 1);
            break;
        case 303:  // Added to the upload queue
            if (sel == 0) proposedNotice(r, photo, "Added  to  upload  queue");
            else if (sel == 1) proposedNotice(r, video, "Added  to  upload  queue");
            else proposedNotice(r, photo, "Already  in  upload  queue");
            break;
        case 304:  // Empty / error
            if (sel == 1) pv::message(r, screen(), "SD card unavailable", "Press the dial to try again.", nullptr);
            else proposedEmpty(r);
            break;
        case 306:  // Loading: opening Photos (splash while the catalog loads)
            currentOpeningSplash(r, "Photos");
            break;
        case 307:  // Loading: library handoff pending
            currentPreparing(r);
            break;
        case 308:  // Loading: video thumbnail (opening on a video, or back from the player)
            currentThumbnailLoading(r);
            break;
        case 309:  // Loading: Videos app before the first frame
            currentVideosLoading(r, sel == 0 ? "Loading videos..." : "Loading preview...");
            break;
        case 310: {  // Full-screen reference layout
            const auto older = pv::formatCapture(2025, 1, 3, 9, 41, 2026);
            if (sel == 1) proposedFullscreen(r, uploaded, 5, 26, nullptr, nullptr);        // no capture time
            else if (sel == 2) proposedFullscreen(r, video, 3, 26, "24 Nov", "4:09 pm");   // video + duration
            else if (sel == 3) proposedFullscreen(r, uploaded, 26, 26, older.date, older.time);
            else proposedFullscreen(r, photo, 2, 26, "24 Nov", "4:12 pm");
            break;
        }
        case 311: {  // Video playback title card
            const pv::VideoCard starting{"VID_0042", "Starting video", "0:14", 0};
            if (sel == 1) proposedVideoCard(r, Sample::Street, pv::VideoCard{"VID_0042", "Playing", "0:01 / 0:14", 80}, 128);
            else if (sel == 2) proposedVideoCard(r, Sample::Street, starting, 0);
            else if (sel == 3) proposedVideoCard(r, Sample::Street, pv::VideoCard{"VID_0042", "Paused", "0:05 / 0:14", 360}, 255);
            else if (sel == 4) proposedVideoCard(r, Sample::Street, pv::VideoCard{"VID_0042", "Ended", "0:14 / 0:14", 1000}, 255);
            else proposedVideoCard(r, Sample::Street, starting, 255);
            break;
        }
        default:  // 305: earlier staging layout, for comparison
            if (sel == 1) currentBrowse(r, Sample::Street, true, "VID_0007.avi", 4, 12);
            else if (sel == 2) currentActions(r, Sample::Sunset, 0);
            else currentBrowse(r, Sample::Sunset, false, "IMG_0042.jpg", 3, 12);
            break;
    }
}

template <class C> void render(C& out, unsigned page, unsigned selection) {
    static photos_study::Raster raster;
    raster = {};
    draw(raster, page, selection);
    emit(out, raster);
}

}  // namespace photos_studio
