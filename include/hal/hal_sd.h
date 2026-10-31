#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <stddef.h>

// SD Card HAL - uses Arduino SD library

namespace hal {
namespace sd {

enum class MountState : uint8_t {
  NotAttempted = 0,
  Mounted,
  Failed,
};

using FileChunkCallback = bool (*)(const uint8_t* data, size_t len, void* userData);

// Opaque sequential reader for large SD-backed media. Each operation owns the
// shared TFT/SD SPI mutex; callers never touch the Arduino SD driver directly.
// Uses the mounted card's unbuffered VFS reads, with no hidden stdio read-ahead.
// Single owner; do not copy a live handle. Paths are absolute within the SD root
// and limited to 256 characters. close resets the entire reader.
struct StreamReader {
  void* handle = nullptr;
  size_t size = 0;
  size_t position = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  // Owning-task diagnostics, read before close resets the reader. Durations
  // separate SPI admission, elapsed driver calls and post-unlock scheduling.
  // A caller-held recursive SPI batch excludes its outer wait from these values.
  // No cross-task snapshots; elapsed microsecond totals wrap after ~71 minutes.
  uint32_t readWaitUs = 0, readIoUs = 0, seekWaitUs = 0, seekIoUs = 0;
  uint32_t readHandoffUs = 0, seekHandoffUs = 0;
  uint32_t readMaxIoUs = 0, seekMaxIoUs = 0;
#endif
};

// Single-owner seekable output. Each operation locks the shared SPI bus; never
// share a writer across tasks. Uses a 4 KiB buffer. Paths are SD-root absolute
// and at most 256 characters. closeWriter always releases a live handle and
// reports prior stream, flush, sync or close errors. Empty close succeeds.
// Success is not a guarantee against power loss during later rename.
struct StreamWriter { void* handle = nullptr; };
bool openWriter(const char* path, StreamWriter* writer);
bool writeStream(StreamWriter* writer, const uint8_t* data, size_t len);
bool seekWriter(StreamWriter* writer, uint32_t position);
bool closeWriter(StreamWriter* writer);

// Initialize SD card using the shared HSPI instance from hal::display.
bool begin(uint32_t frequency = 10000000);

// Check if SD is mounted
bool mounted();

// Last boot/runtime mount result. Consumers must treat anything except
// Mounted as an unavailable optional capability.
MountState mountState();
const char* mountStateName();

// End SD (unmount)
void end();

// Mounted-state-aware existence check. Never touches the shared SPI bus when
// storage is unavailable.
bool exists(const char* path);

// Write binary data to file; returns true if written length == len
bool writeFile(const char* path, const uint8_t* data, size_t len);

// Append binary data to a file; returns true if written length == len.
bool appendFile(const char* path, const uint8_t* data, size_t len);

// Stream a file through a small fixed buffer while the SD bus is owned. This
// is intended for boot-time validation, before the display task starts.
bool streamFile(const char* path, FileChunkCallback callback, void* userData,
                size_t* outSize = nullptr);

bool openStream(const char* path, StreamReader* reader);
size_t readStream(StreamReader* reader, uint8_t* data, size_t len);
bool seekStream(StreamReader* reader, size_t position);
void closeStream(StreamReader* reader);

// Replace destination with a fully-written temporary file. The destination is
// only removed after the temporary file has been verified by the caller.
bool replaceFile(const char* temporaryPath, const char* destinationPath);

// Read up to maxLen-1 bytes of a file into buf and NUL-terminate it. Returns
// bytes read (excluding the NUL), or 0 if missing/empty/error. Intended for
// small text files such as widget scripts.
size_t readFile(const char* path, char* buf, size_t maxLen);

// Remove file; returns true on success
bool removeFile(const char* path);

// Create directory if it doesn't exist (one level; call per segment for
// nested paths). Returns true if the directory exists afterwards.
bool ensureDir(const char* path);

// BMP: read header only (width/height). Returns true if valid BMP.
bool readBmpHeader(const char* path, int32_t* outWidth, int32_t* outHeight);

// BMP: load 24 bpp (BGR) from path into RGB565 buffer (top-down).
// Clips to [0, maxWidth) x [0, maxHeight). Caller allocates imageBuffer.
bool loadBmpFile(const char* path, uint16_t* imageBuffer, int32_t maxWidth, int32_t maxHeight);

// BMP: load a vertical region only (one frame from a tall sprite sheet).
// Loads image rows [startRow, startRow+regionHeight) into imageBuffer (top-down).
// Clips width to min(imgWidth, maxWidth). Use to avoid loading whole file.
// Retries transient SD/read failures (shared SPI, noise).
bool loadBmpRegion(const char* path, uint16_t* imageBuffer, int32_t maxWidth, int32_t regionHeight, int32_t startRow);

// Scan dir for files with given extension (e.g. ".mp3"). Case-insensitive match.
// out is a flat char array: out[i * maxNameLen] = full path of file i.
// Skips subdirs and macOS ._* files. Sorts alphabetically. Returns file count.
int listFiles(const char* dir, const char* ext, char* out, int maxFiles, int maxNameLen);
// Read-only file metadata under the SPI guard; output is valid on success only.
bool fileInfo(const char* path, size_t& bytes, uint32_t& modified);

// Scan dir for subdirectories. out is a flat char array:
// out[i * maxNameLen] = bare directory name (not full path) of dir i.
// Skips files and macOS ._* entries. Sorts alphabetically. Returns dir count.
int listDirs(const char* dir, char* out, int maxDirs, int maxNameLen);

}  // namespace sd
}  // namespace hal
