#include "core/cine_exif.h"
#include <stdio.h>
#include <string.h>

namespace core { namespace cine {
namespace {
class Writer {
    uint8_t* data_;
    size_t capacity_;
public:
    size_t size = 0;
    bool ok = true;
    Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}
    void bytes(const void* data, size_t count) {
        if (!ok || count > capacity_ - size) { ok = false; return; }
        memcpy(data_ + size, data, count);
        size += count;
    }
    void ascii(const char* data, size_t count) {
        bytes(data, count);
        if (count & 1) { const uint8_t zero = 0; bytes(&zero, 1); }
    }
    void u16(uint16_t value) {
        const uint8_t data[] = {uint8_t(value), uint8_t(value >> 8)};
        bytes(data, sizeof(data));
    }
    void u32(uint32_t value) {
        const uint8_t data[] = {uint8_t(value), uint8_t(value >> 8),
                                uint8_t(value >> 16), uint8_t(value >> 24)};
        bytes(data, sizeof(data));
    }
    void entry(uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        u16(tag); u16(type); u32(count); u32(value);
    }
};
constexpr char make[] = "Origami Computing", model[] = "Kami";
constexpr size_t tiffStart = 10; // APP1 marker/length + Exif signature.
}
size_t buildExif(uint8_t* output, size_t capacity, const ExifInfo& info) {
    if (!output || !info.firmwareVersion || !info.colorName || !info.width || !info.height)
        return 0;
    char software[72], description[48];
    const int sn = snprintf(software, sizeof(software), "kamiOS %s / Cine Camera", info.firmwareVersion);
    const int dn = snprintf(description, sizeof(description), "Cine Camera / %s", info.colorName);
    if (sn < 0 || size_t(sn) >= sizeof(software) || dn < 0 || size_t(dn) >= sizeof(description))
        return 0;
    constexpr uint16_t mainCount = 5;
    const uint16_t exifCount = info.exposureUs ? 9 : 8;
    const uint32_t exifOffset = 8 + 2 + mainCount * 12 + 4;
    uint32_t offset = exifOffset + 2 + exifCount * 12 + 4;
    const uint32_t descriptionOffset = offset; offset += (uint32_t(dn + 1) + 1) & ~1u;
    const uint32_t makeOffset = offset; offset += (sizeof(make) + 1) & ~size_t(1);
    const uint32_t modelOffset = offset; offset += (sizeof(model) + 1) & ~size_t(1);
    const uint32_t softwareOffset = offset; offset += (uint32_t(sn + 1) + 1) & ~1u;
    // TIFF out-of-line values begin on even offsets.
    const uint32_t exposureOffset = offset;
    if (info.exposureUs) offset += 8;
    const size_t total = tiffStart + offset;
    if (total > capacity || total > exifCapacity) return 0;
    Writer w(output, capacity);
    const uint8_t prefix[] = {0xff, 0xe1, uint8_t((total - 2) >> 8), uint8_t(total - 2),
                              'E', 'x', 'i', 'f', 0, 0, 'I', 'I'};
    w.bytes(prefix, sizeof(prefix)); w.u16(42); w.u32(8);
    w.u16(mainCount);
    w.entry(0x010e, 2, uint32_t(dn + 1), descriptionOffset);
    w.entry(0x010f, 2, sizeof(make), makeOffset);
    w.entry(0x0110, 2, sizeof(model), modelOffset);
    w.entry(0x0131, 2, uint32_t(sn + 1), softwareOffset);
    w.entry(0x8769, 4, 1, exifOffset);
    w.u32(0); // No thumbnail IFD.
    w.u16(exifCount);
    if (info.exposureUs) w.entry(0x829a, 5, 1, exposureOffset);
    w.entry(0x8822, 3, 1, info.automatic ? 2 : 1); // Normal program / Manual.
    w.entry(0x9000, 7, 4, 0x32333230); // ExifVersion "0232".
    w.entry(0x9101, 7, 4, 0x00030201); // Y, Cb, Cr, unused.
    w.entry(0xa001, 3, 1, 1); // sRGB.
    w.entry(0xa002, 4, 1, info.width);
    w.entry(0xa003, 4, 1, info.height);
    w.entry(0xa402, 3, 1, info.automatic ? 0 : 1);
    w.entry(0xa403, 3, 1, 1); // All Color modes use fixed Sunny WB gains.
    w.u32(0);
    w.ascii(description, size_t(dn + 1)); w.ascii(make, sizeof(make));
    w.ascii(model, sizeof(model)); w.ascii(software, size_t(sn + 1));
    if (info.exposureUs) { w.u32(info.exposureUs); w.u32(1000000); }
    return w.ok && w.size == total ? total : 0;
}
size_t exifInsertionOffset(const uint8_t* jpeg, size_t length) {
    if (!jpeg || length < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
        jpeg[length - 2] != 0xff || jpeg[length - 1] != 0xd9) return 0;
    if (length >= 6 && jpeg[2] == 0xff && jpeg[3] == 0xe0) {
        const size_t bytes = (size_t(jpeg[4]) << 8) | jpeg[5];
        if (bytes < 2 || bytes > length - 6) return 0;
        return 4 + bytes;
    }
    return 2;
}
} }
