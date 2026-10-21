#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ui {
/** Exact before/after damage for a contiguous framebuffer region. UI task only.
 * Owns a PSRAM snapshot; does not draw, retain scenes or suppress callbacks.
 * Allocation failure conservatively damages the whole region. */
class FrameDamage {
public:
    struct Rows { uint16_t first, count; };
    ~FrameDamage() { release(); }
    FrameDamage() = default;
    FrameDamage(const FrameDamage&) = delete;
    FrameDamage& operator=(const FrameDamage&) = delete;
    void begin(const void* pixels, size_t rowBytes, uint16_t rows);
    Rows finish(const void* pixels);
    void release();
private:
    void* before_ = nullptr;
    size_t bytes_ = 0, rowBytes_ = 0;
    uint16_t rows_ = 0;
    bool captured_ = false;
};
}
