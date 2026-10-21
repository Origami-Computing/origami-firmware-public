#include "hal/audio_retirement.h"
#include "services/wav_player.h"

#include <Arduino.h>
#include <SD.h>
#include <cstring>
#include <esp_heap_caps.h>

#include "core/audio_dsp.h"
#include "hal/hal_audio.h"
#include "hal/hal_bus.h"
#include "hal/hal_tasks.h"
#include "services/background_network.h"

namespace services {
namespace audio {

namespace {
constexpr size_t kRingBytes = 48 * 1024;
constexpr int kPcmBufSamples = 1024;  // int16 source samples, mono or interleaved stereo.

uint16_t rd16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void nameFromPath(const char* path, char* out, size_t cap) {
    if (!out || cap == 0) return;
    const char* slash = strrchr(path ? path : "", '/');
    const char* name = slash ? slash + 1 : (path ? path : "");
    strncpy(out, name, cap - 1);
    out[cap - 1] = 0;
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}
}  // namespace

WavPlayer::~WavPlayer() {
    while (!stopTask()) {}  // Keep owner/buffers alive until the worker has finished.
    worker_.cancelCleanup();
    if (sessionOpen_) closeSession();
    else if (fileOpen_) stopTrack();
    if (ring_.buf) heap_caps_free(ring_.buf);
    if (pcmBuf_) heap_caps_free(pcmBuf_);
}

bool WavPlayer::openSession() {
    if (hal::audio::retirementPending()) return false;
    if (sessionOpen_) return sessionReady_;
    sessionOpen_ = true;
    backgroundIoPaused_ = services::background_network::suspendForPeripheralIo(5000);
    if (!backgroundIoPaused_)
        Serial.println("[recordings] ERROR: could not pause background I/O");
    const bool buffersReady = init();
    if (!buffersReady) Serial.println("[recordings] WARNING: buffers unavailable");
    hal::audio::hardwareReset();
    const bool codecReady = hal::audio::initCodec(0x18);
    if (codecReady) hal::audio::restoreForUiPlayback();
    // The worker does no file or I2S work until startTrack sets playing_.
    const bool workerReady = startTask();
    if (!workerReady)
        Serial.println("[recordings] ERROR: WAV task unavailable; internal RAM fragmented");
    sessionReady_ = backgroundIoPaused_ && buffersReady && codecReady && workerReady;
    return sessionReady_;
}

void WavPlayer::closeSession() {
    if (!sessionOpen_) return;
    sessionReady_ = false;
    if (!stopTask()) {
        worker_.deferCleanup([](void* owner) { static_cast<WavPlayer*>(owner)->closeSession(); }, this);
        return;
    }
    worker_.cancelCleanup();
    stopTrack();
    services::audio::media_volume::saveVolume();
    hal::audio::restoreForUiPlayback();
    if (backgroundIoPaused_) {
        backgroundIoPaused_ = false;
        services::background_network::resumeAfterPeripheralIo();
    }
    sessionReady_ = false;
    sessionOpen_ = false;
}

bool WavPlayer::init() {
    services::audio::media_volume::load();
    if (!ring_.buf) {
        void* p = heap_caps_malloc(kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_malloc(kRingBytes, MALLOC_CAP_8BIT);
        if (p) {
            ring_.buf = (int16_t*)p;
            ring_.cap = (int)(kRingBytes / sizeof(int16_t));
            ring_.reset();
            Serial.printf("[wav] ring %u KB (%d samples)\n",
                          (unsigned)(kRingBytes / 1024), ring_.cap);
        } else {
            Serial.println("[wav] ERROR: no PCM ring");
        }
    }
    if (!pcmBuf_) {
        void* p = heap_caps_malloc(kPcmBufSamples * sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_malloc(kPcmBufSamples * sizeof(int16_t), MALLOC_CAP_8BIT);
        pcmBuf_ = (int16_t*)p;
        pcmBufSamples_ = pcmBuf_ ? kPcmBufSamples : 0;
        if (!pcmBuf_) Serial.println("[wav] ERROR: no read buffer");
    }
    return ready();
}

bool WavPlayer::probeTrack(const char* path) {
    if (!path || !path[0]) return false;

    ParsedHeader parsed;
    hal::bus::lockSpi();
    fs::File file = SD.open(path, FILE_READ);
    const bool ok = file && parseHeader_(file, parsed);
    if (file) file.close();
    hal::bus::unlockSpi();
    return ok;
}

bool WavPlayer::startTrack(const char* path) {
    // A failed session-open allocation must never produce a false playing
    // state. Retrying worker admission here is safe; it does not replay input.
    if (!startTask()) return false;
    PlaybackWorker::Control control(worker_);
    if (!control) return false;
    if (worker_.cleanupPending()) return false;
    if (!backgroundIoPaused_) {
        Serial.println("[recordings] Cannot play: background I/O is not paused");
        return false;
    }
    if (!ready()) {
        Serial.println("[wav] Cannot play: buffers missing");
        return false;
    }

    stopTrackUnlocked();
    strncpy(currentPath_, path, sizeof(currentPath_) - 1);
    currentPath_[sizeof(currentPath_) - 1] = 0;

    hal::bus::lockSpi();
    file_ = SD.open(path, FILE_READ);
    if (!file_) {
        hal::bus::unlockSpi();
        Serial.printf("[wav] Cannot open: %s\n", path);
        return false;
    }
    fileOpen_ = true;
    ParsedHeader parsed;
    const bool ok = parseHeader_(file_, parsed);
    hal::bus::unlockSpi();
    if (!ok) {
        Serial.printf("[wav] Unsupported WAV: %s\n", path);
        stopTrackUnlocked();
        return false;
    }

    info_.hz = parsed.hz;
    info_.channels = parsed.channels;
    info_.durationMs = parsed.durationMs;
    dataStart_ = parsed.dataStart;
    dataEnd_ = parsed.dataEnd;
    bytesPerFrame_ = parsed.bytesPerFrame;
    lastGoodFilePos_ = dataStart_;

    nameFromPath(path, info_.title, sizeof(info_.title));
    progress_.reset();
    wantsNext_ = false;
    ring_.reset();
    clearSpectrum();

    if (!hal::audio::txBegin((uint32_t)info_.hz)) {
        Serial.println("[wav] I2S init failed");
        stopTrackUnlocked();
        return false;
    }

    for (int i = 0; i < 12 && ring_.free() > 2048; ++i) {
        if (!pumpFileToRing_()) break;
    }

    Serial.printf("[wav] START '%s' %dHz ch=%d data=%uB dur=%ums\n",
                  path, info_.hz, info_.channels,
                  (unsigned)(dataEnd_ - dataStart_), (unsigned)info_.durationMs);
    playing_ = true;
    return true;
}

bool WavPlayer::stopTrack() {
    PlaybackWorker::Control control(worker_);
    if (!control) return false;
    stopTrackUnlocked();
    return true;
}

void WavPlayer::stopTrackUnlocked() {
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

bool WavPlayer::restartTrack() {
    PlaybackWorker::Control control(worker_);
    if (!control) return false;
    if (!fileOpen_ || dataStart_ == 0 || bytesPerFrame_ == 0) return false;
    playing_ = false;
    hal::bus::lockSpi();
    if (!file_ && !recoverSdFile_()) {
        hal::bus::unlockSpi();
        return false;
    }
    const bool ok = file_.seek(dataStart_);
    hal::bus::unlockSpi();
    if (!ok) return false;
    ring_.reset();
    progress_.reset();
    lastGoodFilePos_ = dataStart_;
    wantsNext_ = false;
    clearSpectrum();
    for (int i = 0; i < 12 && ring_.free() > 2048; ++i) {
        if (!pumpFileToRing_()) break;
    }
    playing_ = true;
    return true;
}

int WavPlayer::scaledVolumeBasisPoints() const {
    return services::audio::media_volume::scaledVolumeBasisPoints();
}

uint32_t WavPlayer::elapsedMs() const {
    return progress_.elapsedMs();
}

bool WavPlayer::consumeWantsNextTrack() {
    return wantsNext_.exchange(false);
}

bool WavPlayer::parseHeader_(fs::File& file, ParsedHeader& parsed) {
    uint8_t hdr[12];
    if (file.read(hdr, sizeof(hdr)) != (int)sizeof(hdr)) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    bool haveFmt = false;
    bool haveData = false;
    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;

    while (file.available() >= 8) {
        uint32_t chunkPos = file.position();
        if (chunkPos == UINT32_MAX) return false;
        uint8_t ch[8];
        if (file.read(ch, sizeof(ch)) != (int)sizeof(ch)) break;
        uint32_t chunkSize = rd32(ch + 4);
        uint32_t payloadPos = file.position();
        if (payloadPos == UINT32_MAX || chunkSize > UINT32_MAX - payloadPos) return false;
        uint32_t nextPos = payloadPos + chunkSize + (chunkSize & 1u);
        if (nextPos < payloadPos) return false;

        if (memcmp(ch, "fmt ", 4) == 0) {
            if (chunkSize < 16) return false;
            uint8_t fmt[16];
            if (file.read(fmt, sizeof(fmt)) != (int)sizeof(fmt)) return false;
            audioFormat = rd16(fmt + 0);
            channels = rd16(fmt + 2);
            sampleRate = rd32(fmt + 4);
            bitsPerSample = rd16(fmt + 14);
            haveFmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            parsed.dataStart = payloadPos;
            parsed.dataEnd = payloadPos + chunkSize;
            haveData = true;
        }

        if (haveFmt && haveData) break;
        if (nextPos <= chunkPos || !file.seek(nextPos)) break;
    }

    if (!haveFmt || !haveData) return false;
    if (audioFormat != 1 || (channels != 1 && channels != 2) || bitsPerSample != 16) {
        Serial.printf("[wav] format unsupported fmt=%u ch=%u bits=%u\n",
                      (unsigned)audioFormat, (unsigned)channels, (unsigned)bitsPerSample);
        return false;
    }
    if (sampleRate < 8000 || sampleRate > 96000) return false;

    parsed.hz = (int)sampleRate;
    parsed.channels = (int)channels;
    parsed.bytesPerFrame = (uint32_t)channels * sizeof(int16_t);
    const uint32_t fileSize = (uint32_t)file.size();
    if (parsed.dataEnd <= parsed.dataStart || parsed.dataEnd > fileSize) return false;

    const uint32_t dataBytes = parsed.dataEnd - parsed.dataStart;
    if (dataBytes % parsed.bytesPerFrame != 0) return false;
    parsed.durationMs = (uint32_t)((uint64_t)dataBytes * 1000ULL /
                                   ((uint64_t)parsed.bytesPerFrame * sampleRate));
    return parsed.durationMs > 0 && file.seek(parsed.dataStart);
}

bool WavPlayer::recoverSdFile_() {
    if (!currentPath_[0] || dataEnd_ <= dataStart_ || bytesPerFrame_ == 0) return false;

    uint32_t replayPos = lastGoodFilePos_;
    if (replayPos < dataStart_) replayPos = dataStart_;
    if (replayPos > dataEnd_) replayPos = dataEnd_;
    replayPos = dataStart_ +
        ((replayPos - dataStart_) / bytesPerFrame_) * bytesPerFrame_;

    file_.close();
    file_ = SD.open(currentPath_, FILE_READ);
    if (!file_) {
        fileOpen_ = false;
        Serial.printf("[wav] SD recover failed reopen path='%s'\n", currentPath_);
        return false;
    }
    fileOpen_ = true;
    if (!file_.seek(replayPos)) {
        Serial.printf("[wav] SD recover seek failed pos=%u path='%s'\n",
                      (unsigned)replayPos, currentPath_);
        file_.close();
        fileOpen_ = false;
        return false;
    }

    lastGoodFilePos_ = replayPos;
    Serial.printf("[wav] SD recover reopened pos=%u elapsed=%ums path='%s'\n",
                  (unsigned)replayPos, (unsigned)elapsedMs(), currentPath_);
    return true;
}

bool WavPlayer::atEndOfFile_() {
    bool atEnd = false;
    hal::bus::lockSpi();
    if (fileOpen_) {
        if (!file_ && !recoverSdFile_()) {
            hal::bus::unlockSpi();
            return false;
        }
        uint32_t pos = file_.position();
        if (pos == UINT32_MAX && recoverSdFile_()) pos = file_.position();
        atEnd = pos != UINT32_MAX && pos >= dataEnd_;
    }
    hal::bus::unlockSpi();
    return atEnd;
}

bool WavPlayer::pumpFileToRing_() {
    if (!fileOpen_ || !pcmBuf_ || bytesPerFrame_ == 0) return false;

    hal::bus::lockSpi();
    if (!fileOpen_) {
        hal::bus::unlockSpi();
        return false;
    }
    if (!file_ && !recoverSdFile_()) {
        hal::bus::unlockSpi();
        return false;
    }
    uint32_t pos = file_.position();
    if (pos == UINT32_MAX) {
        if (!recoverSdFile_()) {
            hal::bus::unlockSpi();
            return false;
        }
        pos = file_.position();
    }
    if (pos >= dataEnd_) {
        hal::bus::unlockSpi();
        return false;
    }

    const int maxFramesByBuf = pcmBufSamples_ / info_.channels;
    const int maxFramesByRing = ring_.free() / 2;
    int frames = maxFramesByBuf < maxFramesByRing ? maxFramesByBuf : maxFramesByRing;
    if (frames <= 0) {
        hal::bus::unlockSpi();
        return true;
    }

    uint32_t remainBytes = dataEnd_ - pos;
    uint32_t wantBytes = (uint32_t)frames * bytesPerFrame_;
    if (wantBytes > remainBytes) wantBytes = remainBytes - (remainBytes % bytesPerFrame_);
    if (wantBytes == 0) {
        hal::bus::unlockSpi();
        return false;
    }

    int got = file_.read((uint8_t*)pcmBuf_, wantBytes);
    if (got <= 0 && pos < dataEnd_ && recoverSdFile_()) {
        pos = file_.position();
        got = file_.read((uint8_t*)pcmBuf_, wantBytes);
    }
    if (got > 0) {
        uint32_t nextPos = pos + (uint32_t)got;
        if (nextPos <= dataEnd_) lastGoodFilePos_ = nextPos;
    }
    hal::bus::unlockSpi();
    if (got <= 0) return false;

    got -= got % (int)bytesPerFrame_;
    int gotFrames = got / (int)bytesPerFrame_;
    if (gotFrames <= 0) return false;

    analyzeVis_(pcmBuf_, gotFrames, info_.channels);
    progress_.add((uint32_t)gotFrames, info_.hz);
    core::dsp::pushStereoWithVolumeBasisPoints(pcmBuf_, gotFrames, info_.channels,
                                               scaledVolumeBasisPoints(), ring_);
    return true;
}

void WavPlayer::drainToI2S_() {
    int avail = ring_.used();
    if (avail <= 0) return;
    int16_t chunk[512];
    while (avail > 0) {
        int toWrite = avail > 512 ? 512 : avail;
        int got = ring_.pop(chunk, toWrite);
        size_t written = 0;
        hal::audio::txWrite(chunk, (size_t)got, &written, 0);
        int samplesWritten = (int)(written / sizeof(int16_t));
        if (samplesWritten < got) {
            ring_.putBack(got - samplesWritten);
            break;
        }
        avail = ring_.used();
    }
}

void WavPlayer::analyzeVis_(const int16_t* pcm, int frames, int channels) {
    int perBar = frames / kVisBars;
    if (perBar < 1) perBar = 1;
    for (int b = 0; b < kVisBars; b++) {
        int start = b * perBar;
        int end = start + perBar < frames ? start + perBar : frames;
        int32_t peak = 0;
        for (int i = start; i < end; i++) {
            int32_t s = channels == 1 ? pcm[i] : (pcm[i * 2] / 2 + pcm[i * 2 + 1] / 2);
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        uint32_t v = (uint32_t)peak * 255u / 32768u;
        spectrum_[b].store((uint8_t)(v > 255u ? 255u : v), std::memory_order_relaxed);
    }
}

void WavPlayer::taskFn_(void* arg) {
    WavPlayer* self = static_cast<WavPlayer*>(arg);
    while (!self->worker_.stopRequested()) {
        if (!self->worker_.beginIteration()) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        unsigned waitMs = 10;
        if (self->playing_ && self->fileOpen_ && self->ring_.buf) {
            while (self->ring_.free() > 2048) {
                if (!self->pumpFileToRing_()) break;
            }
            self->drainToI2S_();
            const bool atEnd = self->atEndOfFile_();
            if (atEnd && self->ring_.used() <= 0) {
                self->wantsNext_ = true;
            }
            waitMs = 2;
        }
        self->worker_.endIteration();
        vTaskDelay(pdMS_TO_TICKS(waitMs));
    }
    Serial.printf("[wav] task stack high-water=%uB configured=%uB\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                  (unsigned)hal::tasks::spec(hal::tasks::Id::WavPlay).stackBytes);
    self->worker_.finish();
    hal::tasks::recordCurrentStack(hal::tasks::Id::WavPlay);
    hal::tasks::deleteCurrent(hal::tasks::Id::WavPlay);
}

bool WavPlayer::startTask() {
    if (worker_.active()) return worker_.running();
    wantsNext_ = false;
    return worker_.start(hal::tasks::Id::WavPlay, taskFn_, this);
}

bool WavPlayer::stopTask() {
    return worker_.stop();
}

}  // namespace audio
}  // namespace services
