#include "core/wav_writer.h"
#include <SD.h>
#include <cstring>
#include <errno.h>
#include <esp_heap_caps.h>

#include "hal/hal_bus.h"

namespace core {

bool WavWriter::open(const char* path, uint32_t sampleRate, uint16_t channels) {
    close();  // ensure previous file is closed
    if (!path || channels == 0 || sampleRate == 0) return false;
    channels_ = channels;
    totalSamples_ = 0;
    fill_ = 0;
    if(strlen(path)>=sizeof(path_)) return false;
    strlcpy(path_,path,sizeof(path_));

    hal::bus::lockSpi();

    // Mkdir if needed
    // Find last '/' and mkdir the directory
    char dir[128];
    const char* slash = strrchr(path, '/');
    if (slash && slash != path) {
        size_t dlen = (size_t)(slash - path);
        if (dlen < sizeof(dir)) {
            memcpy(dir, path, dlen);
            dir[dlen] = '\0';
            if (!SD.exists(dir)) SD.mkdir(dir);
        }
    }

    file_ = SD.open(path, FILE_WRITE);
    if (!file_) {
        hal::bus::unlockSpi();
        return false;
    }

    uint32_t byteRate   = sampleRate * channels * 2;
    uint16_t blockAlign = (uint16_t)(channels * 2);

    // 44-byte WAV header
    uint8_t hdr[44];
    // RIFF chunk
    hdr[0]='R'; hdr[1]='I'; hdr[2]='F'; hdr[3]='F';
    hdr[4]=0; hdr[5]=0; hdr[6]=0; hdr[7]=0;           // RIFF size placeholder
    hdr[8]='W'; hdr[9]='A'; hdr[10]='V'; hdr[11]='E';
    // fmt chunk
    hdr[12]='f'; hdr[13]='m'; hdr[14]='t'; hdr[15]=' ';
    hdr[16]=16; hdr[17]=0; hdr[18]=0; hdr[19]=0;      // chunk size = 16
    hdr[20]=1;  hdr[21]=0;                              // PCM format = 1
    hdr[22]=(uint8_t)channels; hdr[23]=0;              // channels
    // sample rate (4 bytes LE)
    hdr[24]=(uint8_t)(sampleRate);
    hdr[25]=(uint8_t)(sampleRate >> 8);
    hdr[26]=(uint8_t)(sampleRate >> 16);
    hdr[27]=(uint8_t)(sampleRate >> 24);
    // byte rate (4 bytes LE)
    hdr[28]=(uint8_t)(byteRate);
    hdr[29]=(uint8_t)(byteRate >> 8);
    hdr[30]=(uint8_t)(byteRate >> 16);
    hdr[31]=(uint8_t)(byteRate >> 24);
    // block align (2 bytes LE)
    hdr[32]=(uint8_t)(blockAlign);
    hdr[33]=(uint8_t)(blockAlign >> 8);
    // bits per sample
    hdr[34]=16; hdr[35]=0;
    // data chunk
    hdr[36]='d'; hdr[37]='a'; hdr[38]='t'; hdr[39]='a';
    hdr[40]=0; hdr[41]=0; hdr[42]=0; hdr[43]=0;       // data size placeholder

    if (file_.write(hdr, sizeof(hdr)) != sizeof(hdr)) {
        Serial.printf("[wav_writer] Header write failed: %s\n", path);
        file_.close();
        hal::bus::unlockSpi();
        return false;
    }
    open_ = true;
    hal::bus::unlockSpi();
    return true;
}

bool WavWriter::flushBufLocked() {
    if (fill_ == 0) return true;
    if (!open_ || !file_) return false;
    const size_t expected = fill_;
    errno = 0;
    const size_t written = file_.write(buf_, expected);
    const int writeError = errno;
    fill_ = 0;
    if (written == expected) return true;

    Serial.printf("[wav_writer] SD write failed: wrote=%u expected=%u\n",
                  (unsigned)written, (unsigned)expected);
    Serial.printf("[wav_writer] errno=%d (%s) samples=%lu position=%lu dram=%u largest=%u\n",
                  writeError, strerror(writeError), (unsigned long)totalSamples_,
                  (unsigned long)file_.position(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    file_.close();
    open_ = false;
    return false;
}

bool WavWriter::flushBuf() {
    if (fill_ == 0) return true;
    hal::bus::lockSpi();
    const bool ok = flushBufLocked();
    hal::bus::unlockSpi();
    return ok;
}

bool WavWriter::write(const int16_t* pcm, int n) {
    if (!open_ || !pcm || n <= 0) return false;
    const uint8_t* bytes = (const uint8_t*)pcm;
    size_t remain = (size_t)n * sizeof(int16_t);
    size_t off = 0;
    while (remain > 0) {
        size_t space = kBufBytes - fill_;
        if (space == 0) {
            if (!flushBuf()) return false;
            space = kBufBytes;
        }
        size_t chunk = remain < space ? remain : space;
        memcpy(buf_ + fill_, bytes + off, chunk);
        fill_ += chunk;
        off += chunk;
        remain -= chunk;
        if (fill_ >= kBufBytes / 2 && !flushBuf()) return false;
    }
    totalSamples_ += (uint32_t)n;
    return true;
}

bool WavWriter::close() {
    if (!open_ && !file_) {
        totalSamples_ = 0;
        channels_ = 1;
        fill_ = 0;
        return false;
    }

    hal::bus::lockSpi();
    const bool dataOk = flushBufLocked();
    // write() counts interleaved int16 samples, not frames.
    uint32_t dataSize = totalSamples_ * 2u;
    uint32_t riffSize = dataSize + 36u;
    bool ok=false;
    if (dataOk && open_ && file_) {
        // Patch RIFF and data lengths only after all PCM bytes are durable.
        const bool headerOk = file_.seek(4) && file_.write((uint8_t*)&riffSize, 4) == 4 &&
                              file_.seek(40) && file_.write((uint8_t*)&dataSize, 4) == 4;
        if (!headerOk) Serial.println("[wav_writer] WAV header finalize failed");
        ok=headerOk;
    }
    if (file_) file_.close();
    open_ = false;
    // Arduino File::close/flush cannot report fsync failures. Verify what a
    // newly opened reader can actually see before reporting a saved take.
    if(ok) {
        File verify=SD.open(path_,FILE_READ);
        uint8_t header[44];
        ok=verify && verify.size()==dataSize+44u && verify.read(header,44)==44;
        if(ok) {
            uint32_t actualRiff=0,actualData=0;
            memcpy(&actualRiff,header+4,4); memcpy(&actualData,header+40,4);
            ok=memcmp(header,"RIFF",4)==0 && memcmp(header+8,"WAVE",4)==0 &&
               actualRiff==riffSize && actualData==dataSize;
        }
        if(verify) verify.close();
    }
    Serial.printf("[wav_writer] finalized=%u bytes=%lu path=%s\n",ok,
                  (unsigned long)(dataSize+44u),path_);
    hal::bus::unlockSpi();
    totalSamples_ = 0;
    channels_ = 1;
    fill_ = 0;
    return ok;
}

void WavWriter::buildRawHeader(uint8_t* h, uint32_t sampleRate, uint32_t numSamples) {
    const uint32_t ds = numSamples * 2u;
    const uint32_t cs = 36u + ds;
    const uint32_t br = sampleRate * 2u;
    const uint32_t fs = 16u;
    const uint16_t af = 1, ch = 1, ba = 2, bs = 16;
    memcpy(h,    "RIFF", 4); memcpy(h +  4, &cs, 4);
    memcpy(h+8,  "WAVE", 4);
    memcpy(h+12, "fmt ", 4); memcpy(h + 16, &fs, 4);
    memcpy(h+20, &af,    2); memcpy(h + 22, &ch, 2);
    memcpy(h+24, &sampleRate, 4); memcpy(h + 28, &br, 4);
    memcpy(h+32, &ba,    2); memcpy(h + 34, &bs, 2);
    memcpy(h+36, "data", 4); memcpy(h + 40, &ds, 4);
}

}  // namespace core
