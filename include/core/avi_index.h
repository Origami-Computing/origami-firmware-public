#pragma once
#include "core/mjpeg_avi.h"

namespace core { namespace avi {
// One logical video interval. Holds resolve to their preceding actual JPEG.
struct IndexedFrame { uint32_t offset=0, bytes=0, imageNumber=0; };

// Allocation-free validation of our ordered idx1 records, not a general AVI
// index. Info must come from a successful parse(). Caller lends storage for
// info.frames entries until parsing finishes; append borrows 16 readable bytes.
// Validates complete movie coverage and stream totals before offsets are usable.
// The reader must additionally check each selected on-disk chunk header.
class IndexBuilder {
public:
    IndexBuilder(const Info& info, IndexedFrame* frames, size_t capacity)
        : info_(info), frames_(frames), position_(info.headerBytes),
          valid_(frames && info.frames && info.frames<=kMaxFrames && capacity>=info.frames) {}
    bool append(const uint8_t* entry) {
        if (!valid_ || !entry || entries_>=info_.indexEntries ||
            read32(entry+8)!=position_-info_.headerBytes+4 || (read32(entry+4)&~0x10u))
            return valid_=false;
        uint8_t header[8];memcpy(header,entry,4);memcpy(header+4,entry+12,4);
        Chunk chunk;
        if (!parseChunk(header,position_,info_,chunk)) return valid_=false;
        if (chunk.audio) {
            const uint32_t samples=chunk.bytes/2;
            if (samples>info_.audioSamples-audioSamples_) return valid_=false;
            audioSamples_+=samples;
        } else {
            if (videos_>=info_.frames || (!chunk.bytes && !videos_)) return valid_=false;
            if (chunk.bytes) {
                frames_[videos_].offset=position_;frames_[videos_].bytes=chunk.bytes;
                frames_[videos_].imageNumber=videos_;
            }
            else frames_[videos_]=frames_[videos_-1];
            ++videos_;
        }
        position_=chunk.nextOffset;++entries_;
        return true;
    }
    bool complete() const {
        return valid_ && entries_==info_.indexEntries && videos_==info_.frames &&
            audioSamples_==info_.audioSamples && position_==info_.movieEnd;
    }
private:
    const Info& info_;
    IndexedFrame* frames_;
    uint32_t position_,entries_=0,videos_=0,audioSamples_=0;
    bool valid_;
};
} }
