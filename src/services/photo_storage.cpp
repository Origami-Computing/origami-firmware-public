#include "services/photo_storage.h"
#include "services/uploads.h"
#include "hal/hal_sd.h"
#include "core/cine_exif.h"
#include "core/board_time.h"
#include <stdio.h>
#include <string.h>

namespace services { namespace photos {
bool initialize() {
    if (!hal::sd::ensureMediaMounted()) return false;
    (void)uploads::ensureInit();
    return hal::sd::ensureDir("/photos");
}
bool available() { return hal::sd::mounted(); }
Target prepare() {
    Target target;
    char queuePath[64] = {};
    target.sequence = uploads::queue().generatePath("IMG", ".jpg", queuePath, sizeof(queuePath));
    if (!target.sequence) return {};
    const char* slash = strrchr(queuePath, '/');
    const char* name = slash ? slash + 1 : queuePath;
    const int n = snprintf(target.path, sizeof(target.path), "/photos/%s", name);
    if (!name[0] || n < 0 || (size_t)n >= sizeof(target.path) || hal::sd::exists(target.path)) return {};
    return target;
}
void markCaptured(Target& target) {
    target.capturedAtUnix = core::time::isSynced() ? static_cast<uint32_t>(::time(nullptr)) : 0;
}
bool write(const Target& target, const uint8_t* jpeg, size_t length) {
    if (!target.valid()) return false;
    const bool saved = hal::sd::writeFile(target.path, jpeg, length);
    if (!saved) hal::sd::removeFile(target.path);
    return saved;
}
bool write(const Target& target, const uint8_t* jpeg, size_t length,
           const uint8_t* exif, size_t exifLength) {
    if (!exifLength) return write(target, jpeg, length);
    const size_t offset = core::cine::exifInsertionOffset(jpeg, length);
    if (!target.valid() || !offset || !exif || exifLength < 10 ||
        exifLength > core::cine::exifCapacity || exif[0] != 0xff || exif[1] != 0xe1 ||
        ((size_t(exif[2]) << 8) | exif[3]) != exifLength - 2 ||
        memcmp(exif + 4, "Exif\0\0", 6)) return false;
    const hal::sd::FilePart parts[] = {{jpeg, offset}, {exif, exifLength},
                                     {jpeg + offset, length - offset}};
    const bool saved = hal::sd::writeFileParts(target.path, parts, 3);
    if (!saved) hal::sd::removeFile(target.path);
    return saved;
}
bool enqueue(const Target& saved) {
    return saved.valid() && uploads::queue().enqueue("image", saved.path, "image/jpeg", false, saved.capturedAtUnix);
}
} }
