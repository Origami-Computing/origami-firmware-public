#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace core { namespace avi {
// Bounded classic AVI: MJPEG, optionally 48 kHz mono PCM16, word-aligned
// chunks and an idx1 index. Legacy silent recordings remain supported.
// The caller bounds sizes below 4 GiB. All output is explicitly little endian.
constexpr uint32_t kHeaderBytes = 224;
constexpr uint32_t kMaxFrames = 9600;
constexpr uint32_t kAudioHeaderBytes = 324;
constexpr uint32_t kAudioRate = 48000, kAudioChunkBytes = 4096;
constexpr uint32_t kJpegFrameBytes = 128 * 1024;
inline bool supportedDimensions(uint32_t width, uint32_t height) {
    return (width==320 && height==240) || (width==640 && height==480) ||
           (width==1280 && height==720);
}
struct Info {
    uint32_t frames=0, intervalUs=0, movieEnd=0, headerBytes=kHeaderBytes;
    uint32_t audioSamples=0, indexEntries=0;
    uint16_t width=0, height=0;
};
inline uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
// Accept only our fixed MJPEG / MJPEG+PCM layouts. The caller must supply the
// actual readable header length; audio parsing never reads beyond 224 bytes
// unless the additional stream header has been fetched. Other AVI is unsupported.
inline bool parse(const uint8_t* p, uint32_t fileBytes, Info& info,
                  size_t availableHeaderBytes = kHeaderBytes) {
    if (!p || availableHeaderBytes < kHeaderBytes || fileBytes < kHeaderBytes + 8 ||
        memcmp(p,"RIFF",4) || memcmp(p+8,"AVI LIST",8) ||
        memcmp(p+20,"hdrlavih",8) || memcmp(p+96,"strlstrh",8) ||
        memcmp(p+108,"vidsMJPG",8) || memcmp(p+164,"strf",4) || memcmp(p+188,"MJPG",4)) return false;
    const uint32_t streams=read32(p+56);
    if (streams!=1 && streams!=2) return false;
    const bool audio=streams==2;
    const uint32_t headerBytes=audio?kAudioHeaderBytes:kHeaderBytes;
    if (availableHeaderBytes<headerBytes || fileBytes<headerBytes+8) return false;
    const uint32_t frames=read32(p+48), width=read32(p+64), height=read32(p+68);
    const uint32_t scale=read32(p+128), rate=read32(p+132);
    const uint32_t movie=read32(p+headerBytes-8);
    if (read32(p+4)!=fileBytes-8 || read32(p+16)!=headerBytes-32 || read32(p+28)!=56 ||
        read32(p+92)!=116 || read32(p+104)!=56 || read32(p+168)!=40 || read32(p+172)!=40 ||
        read32(p+140)!=frames || read32(p+176)!=width || read32(p+180)!=height ||
        p[184]!=1 || p[185]!=0 || p[186]!=24 || p[187]!=0 ||
        !frames || frames>kMaxFrames || !scale || !rate ||
        !supportedDimensions(width,height) ||
        memcmp(p+headerBytes-12,"LIST",4) || memcmp(p+headerBytes-4,"movi",4) ||
        movie<4 || movie>fileBytes-(headerBytes-4)) return false;
    uint32_t audioSamples=0;
    if (audio) {
        audioSamples=read32(p+264);
        if (memcmp(p+212,"LIST",4) || read32(p+216)!=92 || memcmp(p+220,"strlstrh",8) ||
            read32(p+228)!=56 || memcmp(p+232,"auds",4) || read32(p+252)!=2 ||
            read32(p+256)!=kAudioRate*2 || read32(p+260)!=0 || read32(p+276)!=2 ||
            memcmp(p+288,"strf",4) || read32(p+292)!=16 || read32(p+296)!=0x00010001 ||
            read32(p+300)!=kAudioRate || read32(p+304)!=kAudioRate*2 || read32(p+308)!=0x00100002 ||
            !audioSamples || audioSamples>kAudioRate*300) return false;
    }
    const uint64_t interval=uint64_t(scale)*1000000/rate;
    const uint32_t end=headerBytes-4+movie;
    if (interval<1000 || interval>300000000 || fileBytes-end<8) return false;
    const uint32_t indexBytes=fileBytes-end-8, entries=indexBytes/16;
    if (indexBytes%16 || entries<frames || entries>kMaxFrames*2 || (!audio && entries!=frames)) return false;
    info.frames=frames; info.intervalUs=uint32_t(interval); info.movieEnd=end;
    info.headerBytes=headerBytes; info.audioSamples=audioSamples; info.indexEntries=entries;
    info.width=uint16_t(width); info.height=uint16_t(height);
    return true;
}
struct Chunk {
    uint32_t dataOffset=0, bytes=0, nextOffset=0;
    bool audio=false;
};
// Zero-length video chunks hold the previous image for one timeline interval.
// Readers reject a hold before the first image.
// Validate an eight-byte movi chunk header before reading or seeking its payload.
// The two readers share bounds and padding rules; each validates its own stream.
inline bool parseChunk(const uint8_t* p, uint32_t position, const Info& info, Chunk& chunk) {
    if (!p || position<info.headerBytes || position>info.movieEnd || info.movieEnd-position<8) return false;
    const bool audio=!memcmp(p,"01wb",4);
    if (!audio && memcmp(p,"00dc",4)) return false;
    const uint32_t bytes=read32(p+4), dataOffset=position+8;
    if (audio ? (!info.audioSamples || !bytes || bytes>kAudioChunkBytes || (bytes&1)) :
                ((bytes && bytes<4) || bytes>kJpegFrameBytes)) return false;
    if (bytes+(bytes&1)>info.movieEnd-dataOffset) return false;
    chunk.dataOffset=dataOffset;chunk.bytes=bytes;chunk.nextOffset=dataOffset+bytes+(bytes&1);chunk.audio=audio;
    return true;
}
inline void u32(uint8_t* p, uint32_t n) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(n >> (i * 8));
}
inline uint32_t chunkBytes(uint32_t bytes) { return 8 + bytes + (bytes & 1); }
inline void chunk(uint8_t* out, uint32_t bytes, bool audio=false) {
    memcpy(out, audio ? "01wb" : "00dc", 4); u32(out + 4, bytes);
}
inline void index(uint8_t* out, uint32_t offset, uint32_t bytes, bool audio=false) {
    memcpy(out, audio ? "01wb" : "00dc", 4); u32(out + 4, bytes ? 0x10 : 0);
    u32(out + 8, offset); u32(out + 12, bytes);
}
inline void header(uint8_t* out, uint16_t width, uint16_t height,
                   uint32_t frames, uint32_t durationMs, uint32_t movieBytes,
                   uint32_t largestFrame) {
    memset(out, 0, kHeaderBytes);
    const uint32_t usec = frames ? uint32_t(uint64_t(durationMs) * 1000 / frames) : 200000;
    memcpy(out, "RIFF", 4);
    u32(out + 4, kHeaderBytes + movieBytes + 8 + frames * 16 - 8);
    memcpy(out + 8, "AVI LIST", 8); u32(out + 16, 192);
    memcpy(out + 20, "hdrlavih", 8); u32(out + 28, 56);
    u32(out + 32, usec); u32(out + 44, 0x10); // AVIF_HASINDEX
    u32(out + 48, frames); u32(out + 56, 1); u32(out + 60, largestFrame);
    u32(out + 64, width); u32(out + 68, height);
    memcpy(out + 88, "LIST", 4); u32(out + 92, 116);
    memcpy(out + 96, "strlstrh", 8); u32(out + 104, 56);
    memcpy(out + 108, "vidsMJPG", 8);
    u32(out + 128, durationMs ? durationMs : 1); // scale / rate = seconds/frame
    u32(out + 132, frames ? frames * 1000 : 5);
    u32(out + 140, frames); u32(out + 144, largestFrame); u32(out + 148, 0xffffffff);
    out[160] = uint8_t(width); out[161] = uint8_t(width >> 8);
    out[162] = uint8_t(height); out[163] = uint8_t(height >> 8);
    memcpy(out + 164, "strf", 4); u32(out + 168, 40); u32(out + 172, 40);
    u32(out + 176, width); u32(out + 180, height);
    out[184] = 1; out[186] = 24; memcpy(out + 188, "MJPG", 4);
    u32(out + 192, uint32_t(width) * height * 3);
    memcpy(out + 212, "LIST", 4); u32(out + 216, movieBytes + 4);
    memcpy(out + 220, "movi", 4);
}
// Output must hold kAudioHeaderBytes. indexEntries includes both streams.
inline void audioHeader(uint8_t* out, uint16_t width, uint16_t height,
                        uint32_t frames, uint32_t durationMs, uint32_t movieBytes,
                        uint32_t largestFrame, uint32_t audioSamples, uint32_t indexEntries) {
    header(out,width,height,frames,durationMs,movieBytes,largestFrame);
    memmove(out+312,out+212,12);
    memset(out+212,0,100);
    u32(out+4,kAudioHeaderBytes+movieBytes+indexEntries*16);
    u32(out+16,292); u32(out+56,2);
    memcpy(out+212,"LIST",4); u32(out+216,92);
    memcpy(out+220,"strlstrh",8); u32(out+228,56); memcpy(out+232,"auds",4);
    u32(out+252,2); u32(out+256,kAudioRate*2); u32(out+264,audioSamples);
    u32(out+268,kAudioChunkBytes); u32(out+272,0xffffffff); u32(out+276,2);
    memcpy(out+288,"strf",4); u32(out+292,16);
    u32(out+296,0x00010001); // WAVE_FORMAT_PCM, mono
    u32(out+300,kAudioRate); u32(out+304,kAudioRate*2);
    u32(out+308,0x00100002); // block align 2, 16 bits/sample
}
} }
