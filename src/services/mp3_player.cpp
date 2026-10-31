#include "core/usage_events.h"
#include "hal/audio_retirement.h"
#include "services/mp3_player.h"

#include <Arduino.h>
#include <SD.h>
#include <cstring>
#include <soc/soc_memory_types.h>

#include "hal/hal_audio.h"
#include "hal/hal_bus.h"
#include "hal/hal_tasks.h"
#include "services/background_network.h"

namespace services {
namespace audio {

Mp3Player::~Mp3Player() {
    // Destruction cannot defer the owner's lifetime. Normal app exit uses closeSession.
    while (!stopTask()) {}  // A stuck worker must never outlive freed owner state.
    worker_.cancelCleanup();
    if (sessionOpen_) closeSession();
    else if (fileOpen_) stopTrack();
    if (ring_.buf) {
        heap_caps_free(ring_.buf);
        ring_.buf = nullptr;
        ring_.cap = 0;
    }
    if (inBuf_) {
        heap_caps_free(inBuf_);
        inBuf_ = nullptr;
    }
}

bool Mp3Player::openSession() {
    if (hal::audio::retirementPending()) return false;
    if (sessionOpen_) return sessionReady_;
    sessionOpen_ = true;
    // Preserve the established Music policy while owning its lifetime here.
    // Historically a pause timeout is logged by the queue but does not prevent
    // entering Music; changing that user-visible failure policy is separate.
    const auto suspension = services::background_network::suspendForMusicAudio();
    const bool buffersReady = init();
    if (!buffersReady) Serial.println("[mp3] WARNING: buffers unavailable");
    hal::audio::hardwareReset();
    const bool codecReady = hal::audio::initCodec(0x18);
    if (codecReady) hal::audio::restoreForUiPlayback();
    const bool workerReady = startTask();
    if (!workerReady) {
        Serial.println("[mp3] ERROR: decoder task unavailable; internal RAM fragmented");
    }
    // Periodic-network stop is best-effort for audio; only uploads gate readiness.
    sessionReady_ = suspension.uploadsReady && buffersReady && codecReady && workerReady;
    return sessionReady_;
}

void Mp3Player::closeSession() {
    if (!sessionOpen_) return;
    sessionReady_ = false;
    if (!stopTask()) {
        worker_.deferCleanup([](void* owner) { static_cast<Mp3Player*>(owner)->closeSession(); }, this);
        return;
    }
    worker_.cancelCleanup();
    stopTrack();
    services::audio::media_volume::saveVolume();
    hal::audio::restoreForUiPlayback();
    services::background_network::resumeAfterMusicAudio();
    sessionReady_ = false;
    sessionOpen_ = false;
}

// ---------------------------------------------------------------------------
// Buffer allocation
// ---------------------------------------------------------------------------

bool Mp3Player::init() {
    services::audio::media_volume::load();
    static const size_t kRingTry[] = {
        128 * 1024, 48 * 1024, 32 * 1024, 24 * 1024, 16 * 1024
    };

    if (!inBuf_) {
        inBuf_ = (uint8_t*)core::audio::allocPsramWithFallback(16384);
        if (inBuf_) {
            codec_.init(inBuf_, 16384);
        } else {
            Serial.println("[mp3] ERROR: no input buffer");
        }
    }
    if (!ring_.buf) {
        // Try every supported ring size in PSRAM before spending internal RAM.
        // A smaller external ring leaves internal blocks for DMA/task/TLS users.
        for (int pool = 0; pool < 2 && !ring_.buf; ++pool) {
            for (size_t bytes : kRingTry) {
                void* p = heap_caps_malloc(bytes, pool == 0
                    ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                    : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!p && pool == 0) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
                if (!p) continue;
                ring_.buf = (int16_t*)p;
                ring_.cap = (int)(bytes / sizeof(int16_t));
                ring_.reset();
                Serial.printf("[mp3] ring %u KB (%d samples) PSRAM free=%u KB\n",
                              (unsigned)(bytes / 1024), ring_.cap,
                              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
                break;
            }
        }
        if (!ring_.buf) Serial.println("[mp3] ERROR: no PCM ring");
    }
    // Placement, not free-space totals, establishes whether fallback used scarce
    // internal memory. Log only at session preparation, outside the decode loop.
    const auto location = [](const void* p) {
        return !p ? "none" : esp_ptr_external_ram(p) ? "psram" : "internal";
    };
    Serial.printf("[mp3-buffers] input=%s input_bytes=%u ring=%s ring_bytes=%u psram_largest=%u internal_largest=%u\n",
                  location(inBuf_), inBuf_ ? 16384u : 0u,
                  location(ring_.buf), (unsigned)(ring_.cap * sizeof(int16_t)),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return inBuf_ && ring_.buf;
}

// ---------------------------------------------------------------------------
// Track lifecycle
// ---------------------------------------------------------------------------

bool Mp3Player::startTrack(const char* path) {
    // A failed session-open allocation must never produce a false playing
    // state. Retrying worker admission here is safe; it does not replay input.
    if (!startTask()) return false;
    PlaybackWorker::Control control(worker_);
    if (!control) return false;
    if (worker_.cleanupPending()) return false;
    if (!inBuf_ || !ring_.buf) {
        Serial.println("[mp3] Cannot play: buffers missing");
        return false;
    }

    if (fileOpen_) stopTrackUnlocked();
    strncpy(currentPath_, path, sizeof(currentPath_) - 1);
    currentPath_[sizeof(currentPath_) - 1] = 0;

    hal::bus::lockSpi();
    file_ = SD.open(path);
    if (!file_) {
        hal::bus::unlockSpi();
        Serial.printf("[mp3] Cannot open: %s\n", path);
        return false;
    }
    fileSize_ = file_.size();
    fileOpen_ = true;

    if (!parseID3_()) {
        file_.close();
        fileOpen_ = false;
        hal::bus::unlockSpi();
        Serial.println("[mp3] Invalid or unreadable ID3 metadata");
        return false;
    }
    lastGoodFilePos_ = file_.position();
    hal::bus::unlockSpi();

    if (!info_.title[0]) {
        // Derive a display name from the path (strip directory and extension).
        const char* slash = strrchr(path, '/');
        const char* name  = slash ? slash + 1 : path;
        strncpy(info_.title, name, sizeof(info_.title) - 1);
        info_.title[sizeof(info_.title) - 1] = 0;
        char* dot = strrchr(info_.title, '.');
        if (dot) *dot = 0;
    }

    codec_.reset();
    ring_.reset();
    progress_.reset();
    decodeNoProgressCount_ = 0;

    hal::bus::lockSpi();
    codec_.fillInput(file_);
    lastGoodFilePos_ = file_.position();
    hal::bus::unlockSpi();
    mp3dec_frame_info_t probe;
    int samples = codec_.decodeFrame(decodeBuf_, MINIMP3_MAX_SAMPLES_PER_FRAME, &probe);

    if (probe.hz == 0) {
        Serial.println("[mp3] Cannot detect format");
        hal::bus::lockSpi();
        file_.close();
        hal::bus::unlockSpi();
        fileOpen_ = false;
        return false;
    }

    detectedRate_        = probe.hz;
    info_.hz             = probe.hz;
    info_.channels       = probe.channels;
    info_.bitrateKbps    = (uint32_t)probe.bitrate_kbps;
    estBitrate_          = (uint32_t)probe.bitrate_kbps * 1000u;
    info_.durationMs     = estBitrate_ > 0
                               ? (uint32_t)((uint64_t)(fileSize_ - audioStart_) * 8000u / estBitrate_)
                               : 0;

    Serial.printf("[mp3] START '%s' %dHz ch=%d %dkbps %uB est=%ums\n",
                  path, probe.hz, probe.channels, probe.bitrate_kbps,
                  (unsigned)fileSize_, (unsigned)info_.durationMs);

    if (!hal::audio::txBegin((uint32_t)probe.hz)) {
        Serial.println("[mp3] I2S init failed");
        hal::bus::lockSpi();
        file_.close();
        hal::bus::unlockSpi();
        fileOpen_ = false;
        return false;
    }

    if (samples > 0) {
        progress_.add(samples, detectedRate_);

        core::dsp::pushStereoWithVolumeBasisPoints(decodeBuf_, samples, probe.channels,
                                                   scaledVolumeBasisPoints(), ring_);
    }
    for (int i = 0; i < 20 && ring_.free() > MINIMP3_MAX_SAMPLES_PER_FRAME * 2; i++) {
        if (decodeOneFrame_() <= 0) break;
    }

    playing_ = true;
    usageTrackActive_ = true;
    core::usage::emit("music","mp3.start");
    return true;
}

bool Mp3Player::stopTrack() {
    PlaybackWorker::Control control(worker_);
    if (!control) return false;
    stopTrackUnlocked();
    return true;
}

void Mp3Player::stopTrackUnlocked() {
    if (usageTrackActive_) core::usage::emit("music","mp3.stop");
    usageTrackActive_ = false;
    playing_ = false;
    hal::audio::txEnd();
    if (fileOpen_) {
        hal::bus::lockSpi();
        file_.close();
        hal::bus::unlockSpi();
        fileOpen_ = false;
    }
    ring_.reset();
    currentPath_[0] = 0;
}

int Mp3Player::scaledVolume() const {
    return (scaledVolumeBasisPoints() + 50) / 100;
}

int Mp3Player::scaledVolumeBasisPoints() const {
    return services::audio::media_volume::scaledVolumeBasisPoints();
}

void Mp3Player::seekRelativeSeconds(int seconds) {
    PlaybackWorker::Control control(worker_);
    if (!control) return;
    if (!fileOpen_ || detectedRate_ <= 0 || !inBuf_) return;

    uint32_t curMs    = elapsedMs();
    int32_t  targetMs = (int32_t)curMs + seconds * 1000;
    if (targetMs < 0) targetMs = 0;
    if (info_.durationMs > 0 && (uint32_t)targetMs > info_.durationMs)
        targetMs = info_.durationMs > 1000 ? (int32_t)info_.durationMs - 1000 : 0;

    uint32_t targetByte = 0;
    if (estBitrate_ > 0) {
        targetByte = (uint32_t)((uint64_t)targetMs * estBitrate_ / 8000u);
    } else if (info_.durationMs > 0) {
        targetByte = (uint32_t)((uint64_t)targetMs * (fileSize_ - audioStart_) / info_.durationMs);
    } else {
        return;
    }
    const uint32_t audioBytes = fileSize_ - audioStart_;
    if (targetByte >= audioBytes)
        targetByte = audioBytes > 4096 ? audioBytes - 4096 : 0;
    targetByte += audioStart_;

    hal::bus::lockSpi();
    file_.seek(targetByte);
    lastGoodFilePos_ = file_.position();
    hal::bus::unlockSpi();
    codec_.reset();
    ring_.reset();
    decodeNoProgressCount_ = 0;
    progress_.seek((uint32_t)targetMs, detectedRate_);

    hal::bus::lockSpi();
    codec_.syncPastGarbage(file_, 5);
    hal::bus::unlockSpi();

    for (int i = 0; i < 10 && ring_.free() > MINIMP3_MAX_SAMPLES_PER_FRAME * 2; i++) {
        if (decodeOneFrame_() <= 0) break;
    }
}

uint32_t Mp3Player::elapsedMs() const {
    return progress_.elapsedMs();
}

bool Mp3Player::consumeWantsNextTrack() {
    return wantsNext_.exchange(false);
}

bool Mp3Player::atEndOfCompressedStream_() {
    if (!fileOpen_) return false;
    // Still have compressed bytes on SD or in the staging buffer — not done yet.
    if (file_.available() > 0) return false;
    if (codec_.pendingBytes() >= 16) return false;
    uint32_t pos = file_.position();
    if (pos == UINT32_MAX) return false;
    if (fileSize_ > 0 && pos + (uint32_t)codec_.pendingBytes() < fileSize_) {
        return false;
    }
    return true;
}

bool Mp3Player::recoverSdFile_() {
    if (!currentPath_[0] || fileSize_ == 0) return false;

    uint32_t replayPos = lastGoodFilePos_;
    int pending = codec_.pendingBytes();
    if (pending > 0 && replayPos > (uint32_t)pending) {
        replayPos -= (uint32_t)pending;
    }
    if (replayPos >= fileSize_) {
        replayPos = fileSize_ > 4096 ? fileSize_ - 4096 : 0;
    }

    file_.close();
    file_ = SD.open(currentPath_, FILE_READ);
    if (!file_) {
        fileOpen_ = false;
        Serial.printf("[mp3] SD recover failed reopen path='%s'\n", currentPath_);
        return false;
    }
    if (replayPos > 0 && !file_.seek(replayPos)) {
        Serial.printf("[mp3] SD recover seek failed pos=%u path='%s'\n",
                      (unsigned)replayPos, currentPath_);
        file_.close();
        fileOpen_ = false;
        return false;
    }

    codec_.reset();
    lastGoodFilePos_ = file_.position();
    decodeNoProgressCount_ = 0;
    Serial.printf("[mp3] SD recover reopened pos=%u elapsed=%ums path='%s'\n",
                  (unsigned)lastGoodFilePos_, (unsigned)elapsedMs(), currentPath_);
    return true;
}

// ---------------------------------------------------------------------------
// Decode + drain (called from task on core 0)
// ---------------------------------------------------------------------------

int Mp3Player::decodeOneFrame_() {
    if (!codec_.hasInput() || !ring_.buf || ring_.cap <= 0) return 0;

    // SD and TFT share one SPI bus. Hold the application-level SPI mutex for
    // the SD read so the display flush on core 1 cannot interleave with it.
    // decodeFrame() and drainToI2S() are pure CPU / I2S — no SPI, no lock needed.
    hal::bus::lockSpi();
    codec_.fillInput(file_);
    uint32_t posAfterFill = file_.position();
    if (posAfterFill != UINT32_MAX) {
        lastGoodFilePos_ = posAfterFill;
    } else {
        recoverSdFile_();
    }
    hal::bus::unlockSpi();

    mp3dec_frame_info_t info;
    int samples = codec_.decodeFrame(decodeBuf_, MINIMP3_MAX_SAMPLES_PER_FRAME, &info);
    if (samples <= 0) {
        if (samples == 0 && info.frame_bytes == 0) {
            hal::bus::lockSpi();
            int rd = codec_.fillInput(file_, true);
            uint32_t posAfterForceFill = file_.position();
            if (posAfterForceFill != UINT32_MAX) {
                lastGoodFilePos_ = posAfterForceFill;
            } else if (recoverSdFile_()) {
                rd = codec_.fillInput(file_, true);
                uint32_t posAfterRecoverFill = file_.position();
                if (posAfterRecoverFill != UINT32_MAX) {
                    lastGoodFilePos_ = posAfterRecoverFill;
                }
            }
            hal::bus::unlockSpi();

            if (rd > 0) {
                decodeNoProgressCount_ = 0;
            } else if (!atEndOfCompressedStream_() && ++decodeNoProgressCount_ >= 3) {
                codec_.discardBytes(1);
                decodeNoProgressCount_ = 0;
                Serial.printf("[mp3] decode stall recovery elapsed=%ums sd=%u/%u pending=%d\n",
                              (unsigned)elapsedMs(), (unsigned)lastGoodFilePos_,
                              (unsigned)fileSize_, codec_.pendingBytes());
            }
        } else {
            decodeNoProgressCount_ = 0;
        }
        return samples;
    }

    int stereoCount = samples * 2;
    if (ring_.free() < stereoCount) return 0;

    decodeNoProgressCount_ = 0;

    progress_.add(samples, detectedRate_);
    core::dsp::pushStereoWithVolumeBasisPoints(decodeBuf_, samples, info.channels,
                                               scaledVolumeBasisPoints(), ring_);
    return stereoCount;
}

void Mp3Player::drainToI2S_() {
    if (!ring_.buf || ring_.cap <= 0) return;
    int avail = ring_.used();
    if (avail <= 0) return;

    static uint32_t s_lastGapMs = 0;
    bool draggedEmpty = false;

    int16_t chunk[512];
    while (avail > 0) {
        int    toWrite = avail > 512 ? 512 : avail;
        int    got     = ring_.pop(chunk, toWrite);
        size_t written = 0;
        hal::audio::txWrite(chunk, (size_t)got, &written, 0);
        int samplesWritten = (int)(written / sizeof(int16_t));
        if (samplesWritten < got) {
            ring_.putBack(got - samplesWritten);
            break;
        }
        avail = ring_.used();
        if (avail == 0) draggedEmpty = true;
    }

    if (playing_ && draggedEmpty) {
        uint32_t now = millis();
        if (now - s_lastGapMs > 250) {
            s_lastGapMs = now;
            Serial.printf("[mp3] GAP ring->0 cap=%d elapsed=%ums sd=%u/%u\n",
                          ring_.cap, (unsigned)elapsedMs(),
                          (unsigned)lastGoodFilePos_, (unsigned)fileSize_);
        }
    }
}

// ---------------------------------------------------------------------------
// ID3v2 tag parser — best-effort TIT2 / TPE1 extraction.
// ---------------------------------------------------------------------------

bool Mp3Player::parseID3_() {
    info_.title[0]  = 0;
    info_.artist[0] = 0;
    audioStart_ = 0;
    if (!file_.seek(0)) return false;

    uint8_t hdr[10];
    if (file_.read(hdr, 10) != 10 ||
        hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        return file_.seek(0);
    }
    // ID3v2.2/2.3/2.4 share a synchsafe tag size. Never mask malformed
    // lengths into a plausible offset or seek beyond the actual SD file.
    const uint8_t version = hdr[3];
    if (version < 2 || version > 4 || hdr[4] == 0xFF ||
        ((hdr[6] | hdr[7] | hdr[8] | hdr[9]) & 0x80)) return false;
    uint32_t tagSize = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                       ((uint32_t)(hdr[7] & 0x7F) << 14) |
                       ((uint32_t)(hdr[8] & 0x7F) << 7)  |
                        (uint32_t)(hdr[9] & 0x7F);
    uint32_t tagEnd = 10 + tagSize;
    const uint32_t footerSize = version == 4 && (hdr[5] & 0x10) ? 10 : 0;
    if (tagEnd > fileSize_ || footerSize > fileSize_ - tagEnd) return false;
    audioStart_ = tagEnd + footerSize;
    if (audioStart_ >= fileSize_) return false;
    uint32_t pos    = 10;

    // Text extraction is best-effort. Older, unsynchronised or extended tags
    // still skip correctly, without interpreting their different frame layout.
    // Bound UI-task work even for a file containing thousands of tiny frames.
    for (unsigned frames = 0; version >= 3 && !(hdr[5] & 0xC0) &&
         frames < 128 && tagEnd - pos >= 10; ++frames) {
        uint8_t fhdr[10];
        if (!file_.seek(pos)) return false;
        if (file_.read(fhdr, 10) != 10) break;
        if (fhdr[0] == 0) break;

        char     frameId[5] = { (char)fhdr[0], (char)fhdr[1],
                                 (char)fhdr[2], (char)fhdr[3], 0 };
        uint32_t frameSize  = ((uint32_t)fhdr[4] << 24) | ((uint32_t)fhdr[5] << 16) |
                              ((uint32_t)fhdr[6] <<  8) |  (uint32_t)fhdr[7];
        if (version == 4) {
            if ((fhdr[4] | fhdr[5] | fhdr[6] | fhdr[7]) & 0x80) break;
            frameSize = ((uint32_t)fhdr[4] << 21) | ((uint32_t)fhdr[5] << 14) |
                        ((uint32_t)fhdr[6] << 7) | fhdr[7];
        }
        if (frameSize == 0 || frameSize > tagEnd - pos - 10) break;
        if (frameSize > 4096 || fhdr[9] != 0) { pos += 10 + frameSize; continue; }

        bool isTIT2 = (strcmp(frameId, "TIT2") == 0);
        bool isTPE1 = (strcmp(frameId, "TPE1") == 0);

        if (isTIT2 || isTPE1) {
            uint8_t enc;
            file_.read(&enc, 1);
            uint32_t textLen = frameSize - 1;
            if (textLen >= 63) textLen = 62;
            char* dest = isTIT2 ? info_.title : info_.artist;

            if (enc == 0 || enc == 3) {
                file_.read((uint8_t*)dest, textLen);
                dest[textLen] = 0;
            } else if (enc == 1 || enc == 2) {
                uint8_t bom[2];
                if (textLen >= 2) { file_.read(bom, 2); textLen -= 2; }
                uint32_t out = 0;
                for (uint32_t i = 0; i < textLen && out < 62; i += 2) {
                    uint8_t lo, hi;
                    if (file_.read(&lo, 1) != 1 || file_.read(&hi, 1) != 1) break;
                    if (lo == 0 && hi == 0) break;
                    dest[out++] = (lo >= 0x20 && lo < 0x7F && hi == 0) ? (char)lo : '?';
                }
                dest[out] = 0;
            }
            if (info_.title[0] && info_.artist[0]) break;
        }
        pos += 10 + frameSize;
    }
    // Format detection must start at MPEG frames, not at album artwork/padding
    // that can be larger than the fixed 16 KiB decoder input buffer.
    return file_.seek(audioStart_);
}

// ---------------------------------------------------------------------------
// FreeRTOS task
// ---------------------------------------------------------------------------

void Mp3Player::taskFn_(void* arg) {
    Mp3Player* self = static_cast<Mp3Player*>(arg);
    while (!self->worker_.stopRequested()) {
        if (!self->worker_.beginIteration()) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        unsigned waitMs = 10;
        if (self->playing_ && self->fileOpen_ && self->ring_.buf) {
            while (self->ring_.free() > MINIMP3_MAX_SAMPLES_PER_FRAME * 2) {
                int r = self->decodeOneFrame_();
                if (r < 0) {
                    // decodeFrame returns -1 when the staging buffer holds < 16 B.
                    // Only treat that as track end when the file is actually exhausted;
                    // a drained ring mid-song (SD/SPI contention) must not skip tracks.
                    if (self->ring_.used() <= 0 && self->atEndOfCompressedStream_()) {
                        self->wantsNext_ = true;
                    }
                    break;
                }
                if (r == 0) break;
            }
            self->drainToI2S_();
            waitMs = 2;
        }
        self->worker_.endIteration();
        vTaskDelay(pdMS_TO_TICKS(waitMs));
    }
    Serial.printf("[mp3] task stack high-water=%uB configured=%uB\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                  (unsigned)hal::tasks::spec(hal::tasks::Id::Mp3Play).stackBytes);
    self->worker_.finish();
    hal::tasks::recordCurrentStack(hal::tasks::Id::Mp3Play);
    hal::tasks::deleteCurrent(hal::tasks::Id::Mp3Play);
}

bool Mp3Player::startTask() {
    if (worker_.active()) return worker_.running();
    wantsNext_ = false;
    return worker_.start(hal::tasks::Id::Mp3Play, taskFn_, this);
}

bool Mp3Player::stopTask() {
    const bool stopped = worker_.stop();
    if (!stopped && !sessionOpen_) {
        // Low-level diagnostics have no app session to retire their track.
        worker_.deferCleanup([](void* owner) {
            static_cast<Mp3Player*>(owner)->stopTrack();
        }, this);
    }
    return stopped;
}

}  // namespace audio
}  // namespace services

namespace services { namespace audio {
void Mp3Player::pause() { if (playing_.exchange(false)) core::usage::emit("music","mp3.pause"); }
void Mp3Player::resume() {
 if (worker_.running() && !worker_.cleanupPending() && fileOpen_ && !playing_.exchange(true))
  core::usage::emit("music","mp3.resume");
}
} }
