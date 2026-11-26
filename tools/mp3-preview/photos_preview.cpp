// Host driver for photos_study.h: writes each screen as a binary PPM.
// Build + convert with: python tools/mp3-preview/photos_preview.py
#define SCREEN_STUDIO_HOST 1
#include <cstdio>
#include <string>
#include "Fonts/Instrument/data.inc"
#include "photos_study.h"

using namespace photos_study;

static void save(const Raster& r, const std::string& dir, const char* name) {
    const std::string path = dir + "/" + name + ".ppm";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; ++i) {
        int r8, g8, b8; unpack(r.px[i], r8, g8, b8);
        const unsigned char p[3] = {(unsigned char)r8, (unsigned char)g8, (unsigned char)b8};
        fwrite(p, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", name);
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    static Raster r;
    const Media photo{Sample::Sunset, false, "", false};
    const Media photoUp{Sample::Lake, false, "", true};
    const Media video{Sample::Street, true, "0:12", false};

    // Current staging design.
    r = {}; currentBrowse(r, Sample::Sunset, false, "IMG_0042.jpg", 3, 12); save(r, dir, "before-photo");
    r = {}; currentBrowse(r, Sample::Street, true, "VID_0007.avi", 4, 12); save(r, dir, "before-video");
    r = {}; currentActions(r, Sample::Sunset, 0); save(r, dir, "before-actions");

    // Proposal.
    r = {}; proposedBrowse(r, photo, false, 3, 12); save(r, dir, "after-photo");
    r = {}; proposedBrowse(r, photo, true, 3, 12); save(r, dir, "after-photo-scrolling");
    r = {}; proposedBrowse(r, video, false, 4, 12); save(r, dir, "after-video");
    using pv::Icon;
    const pv::SheetRow photoRows[] = {{"Upload", Icon::Upload, false}, {"Delete", Icon::Trash, true}};
    const pv::SheetRow videoRows[] = {{"Play", Icon::Play, false}, {"Upload", Icon::Upload, false}, {"Delete", Icon::Trash, true}};
    r = {}; proposedSheet(r, photo, photoRows, 2, 0, "Photo", 3, 12); save(r, dir, "after-sheet-photo");
    r = {}; proposedSheet(r, video, videoRows, 3, 0, "Video", 4, 12); save(r, dir, "after-sheet-video");
    r = {}; proposedSheet(r, photoUp, photoRows, 2, 0, "Photo", 5, 12); save(r, dir, "after-sheet-uploaded");
    r = {}; proposedSheet(r, photo, photoRows, 2, 1, "Photo", 3, 12); save(r, dir, "after-sheet-delete-focus");
    r = {}; proposedSheet(r, photo, photoRows, 2, 0, "Photo", 3, 12, "Upload queue full"); save(r, dir, "after-sheet-error");
    r = {}; proposedConfirm(r, photo, false); save(r, dir, "after-confirm-delete");
    r = {}; proposedConfirm(r, photo, true); save(r, dir, "after-confirm-delete-focus");
    r = {}; proposedNotice(r, photo, "Added  to  upload  queue"); save(r, dir, "after-queued");
    r = {}; proposedEmpty(r); save(r, dir, "after-empty");
    // Fill-height (cover) variant: no top/bottom bars, sides cropped.
    r = {}; proposedBrowseCover(r, photo, false, 3, 12); save(r, dir, "cover-photo");
    r = {}; proposedBrowseCover(r, photo, true, 3, 12); save(r, dir, "cover-photo-scrolling");
    r = {}; proposedBrowseCover(r, video, false, 4, 12); save(r, dir, "cover-video");
    r = {}; proposedBrowseCover(r, video, true, 4, 12); save(r, dir, "cover-video-scrolling");
    return 0;
}
