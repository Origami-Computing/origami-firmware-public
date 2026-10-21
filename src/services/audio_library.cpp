#include "services/audio_library.h"
#include "core/memory.h"
#include "services/wav_player.h"
#include "hal/hal_sd.h"
#include <Arduino.h>
#include <cstring>
#include <cstdio>

namespace services { namespace audio {
bool Library::ensureStorage() {
  if (paths_) return true;
  paths_ = static_cast<char (*)[kPathSize]>(
      core::memory::allocPsram(kind_ == Kind::Recordings ? "recordings.file_list" : "music.file_list",
                               kCapacity * kPathSize));
  if (!paths_) {
    Serial.printf("[%s] ERROR: file list PSRAM unavailable\n",
                  kind_ == Kind::Recordings ? "recordings" : "mp3");
    return false;
  }
  memset(paths_, 0, kCapacity * kPathSize);
  return true;
}

void Library::close() {
  void* allocation = paths_;
  core::memory::freePsram(allocation);
  paths_ = nullptr;
  count_ = 0;
  directories_ = 0;
}

int Library::load(const char* musicDirectory) {
  directories_ = 0;
  if (kind_ == Kind::Music && (!musicDirectory ||
      (strcmp(musicDirectory, "/music") && strncmp(musicDirectory, "/music/", 7)) ||
      strstr(musicDirectory, "/.") || strstr(musicDirectory, "//") ||
      strlen(musicDirectory) >= kPathSize)) { count_ = 0; return 0; }
  if (!ensureStorage()) {
    count_ = 0;
    return 0;
  }
  const bool recordings = kind_ == Kind::Recordings;
  if (!recordings) {
    const int foundDirs = hal::sd::listDirs(musicDirectory, paths_[0], kCapacity, kPathSize);
    for (int i = 0; i < foundDirs; ++i) {
      char path[kPathSize];
      const int len = snprintf(path, sizeof(path), "%s/%s", musicDirectory, paths_[i]);
      if (len < 0 || len >= kPathSize) continue;
      memcpy(paths_[directories_++], path, len + 1);
    }
    count_ = directories_;
    if (count_ < kCapacity)
      count_ += hal::sd::listFiles(musicDirectory, ".mp3", paths_[count_], kCapacity-count_, kPathSize);
    return count_;
  }
  const int found = hal::sd::listFiles("/recordings", ".wav", paths_[0], kCapacity, kPathSize);
  int playable = 0;
  for (int i = 0; i < found; ++i) {
    if (!services::audio::WavPlayer::probeTrack(paths_[i])) {
      Serial.printf("[recordings] Skipping invalid or empty WAV: %s\n", paths_[i]);
      continue;
    }
    if (playable != i) {
      memmove(paths_[playable], paths_[i], kPathSize);
    }
    ++playable;
  }

  // Timestamp filenames sort chronologically, so reverse the recordings list
  // to put the newest recording at the top without changing generic SD order.
  for (int i = 0; i < playable / 2; ++i) {
    char tmp[kPathSize];
    const int other = playable - 1 - i;
    memcpy(tmp, paths_[i], sizeof(tmp));
    memcpy(paths_[i], paths_[other], kPathSize);
    memcpy(paths_[other], tmp, kPathSize);
  }
  count_ = playable;
  Serial.printf("[recordings] Found %d playable of %d WAV file(s)\n",
                count_, found);
  return count_;
}

void Library::name(int index, char* out, int capacity) const {
  const char* path = paths_[index];
  const char* slash = strrchr(path, '/');
  const char* name = slash ? slash + 1 : path;
  strncpy(out, name, capacity - 1);
  out[capacity - 1] = 0;
  if (isDirectory(index)) return;
  char* dot = strrchr(out, '.');
  if (dot) *dot = 0;
}

} }
