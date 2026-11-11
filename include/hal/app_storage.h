#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hal { namespace app_storage {
enum class Medium : uint8_t { Sd, Flash };
enum class Result : uint8_t { Ok, Missing, Unavailable, Corrupt, TooLarge, Io, Cancelled };
using Cancel = bool (*)(); // worker-thread query, must not block or re-enter
constexpr uint32_t kFlashOffset = 0xC30000, kFlashBytes = 0x3D0000;
// Single admitted storage-worker caller. All operations block. Paths are relative
// to the managed root, never arbitrary device paths. SD operations use its SPI
// guard. Flash mount validates exact geometry and NEVER formats on failure.
bool hasFlashStore(); // True only for a compatible dedicated customapps partition.
Result mount(Medium medium);
// Explicit provisioning only: verifies every byte is erased before formatting.
// Existing, corrupt or nonblank stores are never erased. Idempotent if mounted.
Result initializeBlankFlash(Cancel cancel = nullptr);
Result read(Medium, const char* path, uint8_t* out, size_t capacity, size_t& size);
Result write(Medium, const char* path, const uint8_t*, size_t, Cancel = nullptr);
Result mkdir(Medium, const char* path);
Result remove(Medium, const char* path);
Result listDirs(Medium, const char* path, char* names, size_t count, size_t stride, bool& overflow);
Result freeBytes(Medium, uint64_t& bytes);
// Block generic SD mutation of the managed root or any ancestor. Case-insensitive
// and conservative about noncanonical paths; read/export remains permitted.
bool protectedSdPath(const char* path);
} }
