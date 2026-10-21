#pragma once

namespace services { namespace audio {
// UI-task audio catalog. Owns PSRAM paths and format-specific ordering, not the
// selected row, playback session or deletion confirmation. No background task.
class Library {
public:
    static constexpr int kCapacity = 64;
    static constexpr int kPathSize = 192;
    enum class Kind { Music, Recordings };
    explicit Library(Kind kind) : kind_(kind) {}
    ~Library() { close(); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    // Music lists subfolders first, then MP3s in HAL order within the requested
    // /music subtree (64 combined entries). Recordings scans the first
    // 64 matching directory entries, then validates and reverses
    // the HAL's alphabetical order. Invalid files do not refill the 64 slots.
    // Allocation/read failure and an empty directory all present as count 0.
    // Entries remain valid until load/close; callers require index < count().
    int load(const char* musicDirectory = "/music");
    // Music folders occupy the prefix; remaining entries are playable tracks.
    int directoryCount() const { return directories_; }
    bool isDirectory(int index) const { return index >= 0 && index < directories_; }
    int count() const { return count_; }
    const char* operator[](int index) const { return paths_[index]; }
    // Browser label: copy basename then remove its final extension after truncation.
    // Requires index < count() and capacity > 0.
    void name(int index, char* out, int capacity) const;
    void close();
private:
    bool ensureStorage();
    const Kind kind_;
    char (*paths_)[kPathSize] = nullptr;
    int count_ = 0;
    int directories_ = 0;
};
} }
