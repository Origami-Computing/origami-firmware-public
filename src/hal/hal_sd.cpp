#include "hal/hal_sd.h"
#include <Arduino.h>
#include <SD.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>
#include "Pins.h"
#include "hal/hal_bus.h"
#include "hal/hal_display.h"

namespace hal {
namespace sd {

static bool s_mounted = false;
static MountState s_mountState = MountState::NotAttempted;

static uint16_t read16(File& f) {
  uint16_t result;
  ((uint8_t*)&result)[0] = f.read();
  ((uint8_t*)&result)[1] = f.read();
  return result;
}

static uint32_t read32(File& f) {
  uint32_t result;
  ((uint8_t*)&result)[0] = f.read();
  ((uint8_t*)&result)[1] = f.read();
  ((uint8_t*)&result)[2] = f.read();
  ((uint8_t*)&result)[3] = f.read();
  return result;
}

bool begin(uint32_t frequency) {
  if (s_mounted) return true;
  hal::bus::lockSpi();
  pinMode(SD_CS, OUTPUT);
  pinMode(LCD_CS_PIN, OUTPUT);
  digitalWrite(LCD_CS_PIN, HIGH);
  digitalWrite(SD_CS, HIGH);

  if (!SD.begin(SD_CS, hal::display::spi(), frequency)) {
    // SD.begin() is an optional probe. Fully close a partial VFS/card state,
    // deselect both devices, and restore a known-good TFT transaction before
    // allowing boot to continue.
    SD.end();
    s_mounted = false;
    s_mountState = MountState::Failed;
    digitalWrite(SD_CS, HIGH);
    digitalWrite(LCD_CS_PIN, HIGH);
    hal::display::recoverAfterSharedSpiFailure();
    hal::bus::unlockSpi();
    return false;
  }
  s_mounted = true;
  s_mountState = MountState::Mounted;
  digitalWrite(SD_CS, HIGH);
  digitalWrite(LCD_CS_PIN, HIGH);
  hal::bus::unlockSpi();
  return true;
}

bool mounted() {
  return s_mounted;
}

MountState mountState() { return s_mountState; }

const char* mountStateName() {
  switch (s_mountState) {
    case MountState::Mounted: return "mounted";
    case MountState::Failed: return "failed";
    default: return "not-attempted";
  }
}

void end() {
  hal::bus::lockSpi();
  SD.end();
  s_mounted = false;
  s_mountState = MountState::NotAttempted;
  digitalWrite(SD_CS, HIGH);
  digitalWrite(LCD_CS_PIN, HIGH);
  hal::bus::unlockSpi();
}

bool exists(const char* path) {
  if (!s_mounted || !path) return false;
  hal::bus::lockSpi();
  const bool ok = SD.exists(path);
  hal::bus::unlockSpi();
  return ok;
}

bool writeFile(const char* path, const uint8_t* data, size_t len) {
  if (!s_mounted || !path || !data) return false;
  hal::bus::lockSpi();
  File f = SD.open(path, FILE_WRITE);
  if (!f) { hal::bus::unlockSpi(); return false; }
  size_t written = f.write(data, len);
  f.close();
  hal::bus::unlockSpi();
  return (written == len);
}

bool appendFile(const char* path, const uint8_t* data, size_t len) {
  if (!s_mounted || !path || (!data && len > 0)) return false;
  hal::bus::lockSpi();
  File f = SD.open(path, FILE_APPEND);
  if (!f) { hal::bus::unlockSpi(); return false; }
  const size_t written = len > 0 ? f.write(data, len) : 0;
  f.close();
  hal::bus::unlockSpi();
  return written == len;
}

bool streamFile(const char* path, FileChunkCallback callback, void* userData,
                size_t* outSize) {
  if (outSize) *outSize = 0;
  if (!s_mounted || !path || !callback) return false;

  hal::bus::lockSpi();
  File f = SD.open(path, FILE_READ);
  if (!f) { hal::bus::unlockSpi(); return false; }

  uint8_t buffer[2048];
  size_t total = 0;
  bool ok = true;
  for (;;) {
    const int got = f.read(buffer, sizeof(buffer));
    if (got < 0) {
      ok = false;
      break;
    }
    if (got == 0) break;
    total += (size_t)got;
    if (!callback(buffer, (size_t)got, userData)) {
      ok = false;
      break;
    }
  }
  f.close();
  hal::bus::unlockSpi();
  if (outSize) *outSize = total;
  return ok;
}

bool openStream(const char* path, StreamReader* reader) {
  if (!s_mounted || !path || !reader || reader->handle) return false;
  hal::bus::lockSpi();
  File opened = SD.open(path, FILE_READ);
  if (!opened) {
    hal::bus::unlockSpi();
    return false;
  }
  File* file = new File(opened);
  if (!file) {
    opened.close();
    hal::bus::unlockSpi();
    return false;
  }
  reader->handle = file;
  reader->size = file->size();
  reader->position = 0;
  hal::bus::unlockSpi();
  return true;
}

size_t readStream(StreamReader* reader, uint8_t* data, size_t len) {
  if (!reader || !reader->handle || !data || len == 0) return 0;
  File* file = static_cast<File*>(reader->handle);
  hal::bus::lockSpi();
  const int got = file->read(data, len);
  hal::bus::unlockSpi();
  if (got <= 0) return 0;
  reader->position += (size_t)got;
  return (size_t)got;
}

bool seekStream(StreamReader* reader, size_t position) {
  if (!reader || !reader->handle || position > reader->size) return false;
  File* file = static_cast<File*>(reader->handle);
  hal::bus::lockSpi();
  const bool ok = file->seek(position);
  hal::bus::unlockSpi();
  if (ok) reader->position = position;
  return ok;
}

void closeStream(StreamReader* reader) {
  if (!reader || !reader->handle) return;
  File* file = static_cast<File*>(reader->handle);
  hal::bus::lockSpi();
  file->close();
  hal::bus::unlockSpi();
  delete file;
  *reader = StreamReader{};
}

bool replaceFile(const char* temporaryPath, const char* destinationPath) {
  if (!s_mounted || !temporaryPath || !destinationPath) return false;
  hal::bus::lockSpi();
  if (!SD.exists(temporaryPath)) {
    hal::bus::unlockSpi();
    return false;
  }
  if (SD.exists(destinationPath) && !SD.remove(destinationPath)) {
    hal::bus::unlockSpi();
    return false;
  }
  const bool ok = SD.rename(temporaryPath, destinationPath);
  hal::bus::unlockSpi();
  return ok;
}

size_t readFile(const char* path, char* buf, size_t maxLen) {
  if (!s_mounted || !path || !buf || maxLen == 0) return 0;
  hal::bus::lockSpi();
  File f = SD.open(path, FILE_READ);
  if (!f) { hal::bus::unlockSpi(); return 0; }
  int r = f.read((uint8_t*)buf, maxLen - 1);
  f.close();
  hal::bus::unlockSpi();
  const size_t n = (r > 0) ? (size_t)r : 0;
  buf[n] = '\0';
  return n;
}

bool ensureDir(const char* path) {
  if (!s_mounted || !path) return false;
  hal::bus::lockSpi();
  const bool ok = SD.exists(path) || SD.mkdir(path);
  hal::bus::unlockSpi();
  return ok;
}

bool removeFile(const char* path) {
  if (!s_mounted || !path) return false;
  hal::bus::lockSpi();
  bool ok = SD.remove(path);
  hal::bus::unlockSpi();
  return ok;
}

bool readBmpHeader(const char* path, int32_t* outWidth, int32_t* outHeight) {
  if (!s_mounted || !path || !outWidth || !outHeight) return false;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      delay(4);
      yield();
    }
    hal::bus::lockSpi();
    File f = SD.open(path);
    if (!f) {
      hal::bus::unlockSpi();
      continue;
    }
    if (read16(f) != 0x4D42) {
      f.close();
      hal::bus::unlockSpi();
      return false;
    }
    read32(f);
    read32(f);
    uint32_t dataOffset = read32(f);
    read32(f);
    int32_t w = (int32_t)read32(f);
    int32_t h = (int32_t)read32(f);
    f.close();
    *outWidth = w;
    *outHeight = h < 0 ? -h : h;
    (void)dataOffset;
    hal::bus::unlockSpi();
    return true;
  }
  return false;
}

bool loadBmpFile(const char* path, uint16_t* imageBuffer, int32_t maxWidth, int32_t maxHeight) {
  if (!s_mounted || !path || !imageBuffer) return false;
  hal::bus::lockSpi();
  File f = SD.open(path);
  if (!f) { hal::bus::unlockSpi(); return false; }
  if (read16(f) != 0x4D42) {
    f.close();
    hal::bus::unlockSpi();
    return false;
  }
  read32(f);
  read32(f);
  uint32_t dataOffset = read32(f);
  read32(f);
  int32_t headerWidth = (int32_t)read32(f);
  int32_t headerHeight = (int32_t)read32(f);
  read16(f);
  uint16_t bitsPerPixel = read16(f);
  read32(f);
  read32(f);
  read32(f);
  read32(f);
  read32(f);
  read32(f);
  f.seek(dataOffset);

  if (bitsPerPixel != 24) {
    f.close();
    hal::bus::unlockSpi();
    return false;
  }

  int32_t imgW = headerWidth;
  int32_t imgH = headerHeight < 0 ? -headerHeight : headerHeight;
  int32_t displayWidth = (imgW < maxWidth) ? imgW : maxWidth;
  int32_t displayHeight = (imgH < maxHeight) ? imgH : maxHeight;

  uint8_t bytesPerPixel = 3;
  uint8_t padding = (4 - ((imgW * bytesPerPixel) & 3)) & 3;
  size_t rowSize = (size_t)imgW * bytesPerPixel + padding;
  uint8_t* lineBuffer = (uint8_t*)malloc(rowSize);
  if (!lineBuffer) {
    f.close();
    hal::bus::unlockSpi();
    return false;
  }

  memset(imageBuffer, 0, (size_t)displayWidth * displayHeight * sizeof(uint16_t));

  for (int32_t y = displayHeight - 1; y >= 0; y--) {
    if (f.read(lineBuffer, rowSize) != (int)rowSize) {
      free(lineBuffer);
      f.close();
      hal::bus::unlockSpi();
      return false;
    }
    uint8_t* bptr = lineBuffer;
    for (int32_t x = 0; x < displayWidth; x++) {
      uint16_t color = (uint16_t)((bptr[0] >> 3) | ((bptr[1] & 0xFC) << 3) | ((bptr[2] & 0xF8) << 8));
      imageBuffer[y * displayWidth + x] = color;
      bptr += bytesPerPixel;
    }
  }

  free(lineBuffer);
  f.close();
  hal::bus::unlockSpi();
  return true;
}

// Convert BGR (3 bytes) to RGB565
static inline uint16_t bgrTo565(uint8_t b, uint8_t g, uint8_t r) {
  return (uint16_t)((b >> 3) | ((g & 0xFC) << 3) | ((r & 0xF8) << 8));
}

/** One attempt; caller validates args. Reads the requested rows sequentially after one seek. */
static bool loadBmpRegionOnce(const char* path, uint16_t* imageBuffer, int32_t maxWidth, int32_t regionHeight,
                              int32_t startRow) {
  hal::bus::lockSpi();
  File f = SD.open(path);
  if (!f) {
    hal::bus::unlockSpi();
    return false;
  }
  size_t fileSize = f.size();  // get before reads (some SD libs report 0 after reading)
  if (read16(f) != 0x4D42) {
    f.close();
    Serial.println("loadBmpRegion: not BMP");
    hal::bus::unlockSpi();
    return false;
  }
  read32(f);
  read32(f);
  uint32_t dataOffset = read32(f);
  uint32_t dibSize = read32(f);
  int32_t headerWidth = (int32_t)read32(f);
  int32_t headerHeight = (int32_t)read32(f);
  read16(f);
  uint16_t bitsPerPixel = read16(f);
  read32(f);
  read32(f);
  read32(f);
  read32(f);
  read32(f);
  read32(f);

  if (bitsPerPixel != 24 && bitsPerPixel != 8) {
    f.close();
    Serial.printf("loadBmpRegion: unsupported bpp %u\n", (unsigned)bitsPerPixel);
    hal::bus::unlockSpi();
    return false;
  }

  int32_t imgW = headerWidth;
  bool topDown = (headerHeight < 0);
  int32_t imgH = headerHeight < 0 ? -headerHeight : headerHeight;
  if (startRow + regionHeight > imgH) {
    f.close();
    Serial.printf("loadBmpRegion: region OOB (startRow=%ld + h=%ld > imgH=%ld)\n",
                  (long)startRow, (long)regionHeight, (long)imgH);
    hal::bus::unlockSpi();
    return false;
  }

  int32_t displayWidth = (imgW < maxWidth) ? imgW : maxWidth;
  size_t rowSize;
  uint16_t palette[256];  // 8bpp: palette indices -> RGB565

  if (bitsPerPixel == 8) {
    // Load 256-entry BGR0 palette (starts at 14 + dibSize)
    if (!f.seek(14 + dibSize)) {
      f.close();
      Serial.println("loadBmpRegion: seek to palette failed");
      hal::bus::unlockSpi();
      return false;
    }
    for (int i = 0; i < 256; i++) {
      uint8_t b = f.read();
      uint8_t g = f.read();
      uint8_t r = f.read();
      f.read();  // reserved
      palette[i] = bgrTo565(b, g, r);
    }
    rowSize = ((size_t)imgW + 3) & ~3;  // 4-byte aligned
  } else {
    uint8_t padding = (4 - ((imgW * 3) & 3)) & 3;
    rowSize = (size_t)imgW * 3 + padding;
  }

  // The requested rows are contiguous on disk even for a bottom-up BMP; only
  // their order is reversed. Seek once to the start of that block, read it in
  // large sequential chunks, then map each stored row to its top-down output
  // row. The previous implementation sought once per row and split every
  // 716-byte eye row into three 256-byte reads, turning one frame into hundreds
  // of high-overhead SD operations.
  uint64_t seekPos64;
  if (topDown) {
    seekPos64 = (uint64_t)dataOffset + (uint64_t)startRow * rowSize;
  } else {
    const int32_t regionBottomRow = startRow + regionHeight - 1;
    const uint32_t firstStoredRow = (uint32_t)(imgH - 1 - regionBottomRow);
    seekPos64 = (uint64_t)dataOffset + (uint64_t)firstStoredRow * rowSize;
  }
  const uint64_t regionBytes64 = (uint64_t)rowSize * (uint64_t)regionHeight;
  if (seekPos64 > fileSize || regionBytes64 > (uint64_t)fileSize - seekPos64 ||
      seekPos64 > UINT32_MAX || regionBytes64 > SIZE_MAX) {
    f.close();
    Serial.println("loadBmpRegion: pixel block outside file");
    hal::bus::unlockSpi();
    return false;
  }
  const uint32_t seekPos = (uint32_t)seekPos64;
  if (!f.seek(seekPos)) {
    f.close();
    Serial.printf("loadBmpRegion: seek failed (pos=%u)\n", (unsigned)seekPos);
    hal::bus::unlockSpi();
    return false;
  }

  // Target roughly 16 KiB per File::read while allowing allocation pressure to
  // reduce the chunk all the way to one row without reverting to per-row seeks.
  static constexpr size_t kTargetReadBytes = 16u * 1024u;
  size_t rowsPerChunk = kTargetReadBytes / rowSize;
  if (rowsPerChunk < 1) rowsPerChunk = 1;
  if (rowsPerChunk > (size_t)regionHeight) rowsPerChunk = (size_t)regionHeight;

  uint8_t* chunkBuffer = nullptr;
  while (rowsPerChunk >= 1) {
    chunkBuffer = (uint8_t*)malloc(rowsPerChunk * rowSize);
    if (chunkBuffer || rowsPerChunk == 1) break;
    rowsPerChunk /= 2;
  }
  if (!chunkBuffer) {
    f.close();
    Serial.println("loadBmpRegion: chunk buffer malloc failed");
    hal::bus::unlockSpi();
    return false;
  }

  size_t storedRow = 0;
  while (storedRow < (size_t)regionHeight) {
    size_t chunkRows = (size_t)regionHeight - storedRow;
    if (chunkRows > rowsPerChunk) chunkRows = rowsPerChunk;
    const size_t chunkBytes = chunkRows * rowSize;
    size_t need = chunkBytes;
    uint8_t* dst = chunkBuffer;
    while (need > 0) {
      int got = f.read(dst, need);
      if (got <= 0) {
        free(chunkBuffer);
        f.close();
        Serial.printf("loadBmpRegion: sequential read failed at stored row %u offset %u (got %d)\n",
                      (unsigned)storedRow, (unsigned)(chunkBytes - need), got);
        hal::bus::unlockSpi();
        return false;
      }
      need -= (size_t)got;
      dst += got;
    }

    for (size_t rowInChunk = 0; rowInChunk < chunkRows; rowInChunk++) {
      const size_t fileRow = storedRow + rowInChunk;
      const int32_t outputY = topDown
          ? (int32_t)fileRow
          : regionHeight - 1 - (int32_t)fileRow;
      const uint8_t* row = chunkBuffer + rowInChunk * rowSize;
      if (bitsPerPixel == 8) {
        for (int32_t x = 0; x < displayWidth; x++)
          imageBuffer[(size_t)outputY * displayWidth + x] = palette[row[x]];
      } else {
        const uint8_t* bptr = row;
        for (int32_t x = 0; x < displayWidth; x++) {
          imageBuffer[(size_t)outputY * displayWidth + x] = bgrTo565(bptr[0], bptr[1], bptr[2]);
          bptr += 3;
        }
      }
    }
    storedRow += chunkRows;
  }

  free(chunkBuffer);
  f.close();
  hal::bus::unlockSpi();
  return true;
}

bool loadBmpRegion(const char* path, uint16_t* imageBuffer, int32_t maxWidth, int32_t regionHeight, int32_t startRow) {
  if (!s_mounted || !path || !imageBuffer || regionHeight <= 0 || startRow < 0) return false;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      delay(5);
      yield();
    }
    if (loadBmpRegionOnce(path, imageBuffer, maxWidth, regionHeight, startRow)) return true;
  }
  return false;
}

int listFiles(const char* dir, const char* ext, char* out, int maxFiles, int maxNameLen) {
  if (!s_mounted || !dir || !ext || !out || maxFiles <= 0 || maxNameLen <= 0) return 0;
  hal::bus::lockSpi();
  File d = SD.open(dir);
  if (!d) {
    hal::bus::unlockSpi();
    return 0;
  }
  int count = 0;
  int extLen = (int)strlen(ext);
  while (count < maxFiles) {
    File entry = d.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) { entry.close(); continue; }
    const char* name = entry.name();
    // Skip macOS ._* files (name may be just the basename after Arduino SD)
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (base[0] == '.' && base[1] == '_') { entry.close(); continue; }
    int nameLen = (int)strlen(name);
    if (nameLen < extLen) { entry.close(); continue; }
    if (strcasecmp(name + nameLen - extLen, ext) != 0) { entry.close(); continue; }
    snprintf(out + count * maxNameLen, (size_t)maxNameLen, "%s/%s", dir, name);
    count++;
    entry.close();
  }
  d.close();
  hal::bus::unlockSpi();
  // Bubble sort alphabetically
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (strcasecmp(out + i * maxNameLen, out + j * maxNameLen) > 0) {
        char tmp[256];
        if (maxNameLen <= (int)sizeof(tmp)) {
          memcpy(tmp, out + i * maxNameLen, (size_t)maxNameLen);
          memcpy(out + i * maxNameLen, out + j * maxNameLen, (size_t)maxNameLen);
          memcpy(out + j * maxNameLen, tmp, (size_t)maxNameLen);
        }
      }
    }
  }
  return count;
}

int listDirs(const char* dir, char* out, int maxDirs, int maxNameLen) {
  if (!s_mounted || !dir || !out || maxDirs <= 0 || maxNameLen <= 0) return 0;
  hal::bus::lockSpi();
  File d = SD.open(dir);
  if (!d) {
    hal::bus::unlockSpi();
    return 0;
  }
  int count = 0;
  while (count < maxDirs) {
    File entry = d.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) { entry.close(); continue; }
    const char* name = entry.name();
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (base[0] == '.') { entry.close(); continue; }
    snprintf(out + count * maxNameLen, (size_t)maxNameLen, "%s", base);
    count++;
    entry.close();
  }
  d.close();
  hal::bus::unlockSpi();
  // Bubble sort alphabetically
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (strcasecmp(out + i * maxNameLen, out + j * maxNameLen) > 0) {
        char tmp[256];
        if (maxNameLen <= (int)sizeof(tmp)) {
          memcpy(tmp, out + i * maxNameLen, (size_t)maxNameLen);
          memcpy(out + i * maxNameLen, out + j * maxNameLen, (size_t)maxNameLen);
          memcpy(out + j * maxNameLen, tmp, (size_t)maxNameLen);
        }
      }
    }
  }
  return count;
}

}  // namespace sd
}  // namespace hal
