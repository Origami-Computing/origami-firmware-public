#include "services/photo_storage.h"
#include "services/uploads.h"
#include "hal/hal_sd.h"
#include <stdio.h>
#include <string.h>

namespace services { namespace photos {
bool initialize() {
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
bool write(const Target& target, const uint8_t* jpeg, size_t length) {
    if (!target.valid()) return false;
    const bool saved = hal::sd::writeFile(target.path, jpeg, length);
    if (!saved) hal::sd::removeFile(target.path);
    return saved;
}
bool enqueue(const Target& saved) {
    return saved.valid() && uploads::queue().enqueue("image", saved.path, "image/jpeg", false);
}
} }
