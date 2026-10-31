#include "core/preview_decoder.h"
#include "core/mjpeg_avi.h"
#include <Arduino.h>
#include <JPEGDEC.h>
#include <esp_jpeg_dec.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <limits.h>
#include <new>
#include <string.h>

namespace core { namespace images {
namespace {
// Both embedded codecs misdecode non-unit chroma sampling (for example
// FFmpeg's Y=2x2, Cb/Cr=1x2 4:2:2). Validate the supported baseline layout
// before entering the binary decoder. This is not an entropy-stream validator.
bool supportedVideoJpeg(const uint8_t* data, size_t length) {
    if (length<2 || data[0]!=0xff || data[1]!=0xd8) return false;
    bool frame=false;
    size_t at=2;
    while (at<length) {
        if (data[at++]!=0xff) return false;
        while (at<length && data[at]==0xff) ++at;
        if (at==length) return false;
        const uint8_t marker=data[at++];
        if (!marker || marker==0x01 || (marker>=0xd0 && marker<=0xd9)) return false;
        if (length-at<2) return false;
        const size_t bytes=(size_t(data[at])<<8)|data[at+1];
        if (bytes<2 || bytes>length-at) return false;
        if (marker==0xda) return frame; // Vendor validates scan and entropy data.
        if (marker>=0xc0 && marker<=0xcf && marker!=0xc4) {
            if (marker!=0xc0 || frame || bytes<8 || data[at+2]!=8) return false;
            const unsigned height=(unsigned(data[at+3])<<8)|data[at+4];
            const unsigned width=(unsigned(data[at+5])<<8)|data[at+6];
            if (!core::avi::supportedDimensions(width,height)) return false;
            const unsigned components=data[at+7];
            if (bytes!=8+3*components) return false;
            if (components==1) {
                if (data[at+9]!=0x11) return false;
            } else if (components==3) {
                const uint8_t y=data[at+9];
                if ((y!=0x11 && y!=0x21 && y!=0x22) ||
                    data[at+12]!=0x11 || data[at+15]!=0x11) return false;
            } else return false;
            frame=true;
        }
        at+=bytes;
    }
    return false;
}
struct Decode {
    uint16_t* pixels;
    PreviewDecoder::ShouldAbort shouldAbort;
    void* user;
    bool interrupted;
    bool fit;
    int sourceHeight;
    int outputHeight;
    int top;
    uint16_t sourceX[280];
    uint16_t sourceY[240];
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    uint32_t writeUs, blocks;
#endif
};
int writeBlock(JPEGDRAW* block) {
    Decode& decode = *static_cast<Decode*>(block->pUser);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    const uint32_t started=micros();
    struct BlockTiming {
        Decode& decode; uint32_t started;
        ~BlockTiming() {decode.writeUs+=uint32_t(micros()-started);++decode.blocks;}
    } timing{decode,started};
#endif
    if (decode.shouldAbort && decode.shouldAbort(decode.user)) {
        decode.interrupted = true;
        return 0;
    }
    const int x = block->x, y = block->y;
    const int w = block->iWidthUsed, h = block->iHeight;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 320 || y + h > decode.sourceHeight) return 0;
    if (decode.fit) {
        // Full 4:3 image fitted to 280x210, letterboxed in the 280x240 viewport.
        const int fitX0=(x*280+319)/320, fitX1=std::min(280,((x+w)*280+319)/320);
        const int fitY0=(y*decode.outputHeight+decode.sourceHeight-1)/decode.sourceHeight;
        const int fitY1=std::min(decode.outputHeight,((y+h)*decode.outputHeight+decode.sourceHeight-1)/decode.sourceHeight);
        for (int dx=fitX0; dx<fitX1; ++dx) {
            const int sx=decode.sourceX[dx]-x;
            uint16_t* dst=decode.pixels+(279-dx)*240+decode.top;
            for (int dy=fitY0;dy<fitY1;++dy) {
                const int sy=decode.sourceY[dy]-y;
                dst[dy]=block->pPixels[sy*block->iWidth+sx];
            }
        }
        return 1;
    }
    const int x0 = std::max(0, x - 20);
    const int x1 = std::min(280, x + w - 20);
    // Preserve the fused center crop + portrait transpose. Destination columns
    // are contiguous in PSRAM; source MCU rows retain the decoder's full stride.
    for (int dx = x0; dx < x1; ++dx) {
        uint16_t* dst = decode.pixels + (279 - dx) * 240;
        const int sx = decode.sourceX[dx] - x;
        for (int dy = y; dy < y + h; ++dy)
            dst[dy] = block->pPixels[(decode.sourceY[dy] - y) * block->iWidth + sx];
    }
    return 1;
}
}
PreviewDecoder::~PreviewDecoder() { close(); }
void PreviewDecoder::close() {
    if (videoDecoder_) jpeg_dec_close(videoDecoder_);
    videoDecoder_=nullptr;cameraCrop_=false;reducedVideo_=false;
    heap_caps_free(videoPixels_);videoPixels_=nullptr;videoPixelBytes_=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    profileCount_=profileTotalUs_=profileWriteUs_=profileBlocks_=profileBytes_=0;
#endif
    if (!decoder_) return;
    decoder_->~JPEGDEC();
    heap_caps_free(decoder_);
    decoder_ = nullptr;
}
bool PreviewDecoder::prepare() {
    close();
    void* workspace = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const bool internal = workspace != nullptr;
    if (!workspace) workspace = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!workspace) return false;
    decoder_ = new (workspace) JPEGDEC;
    Serial.printf("[camera-vga] decoder workspace=%s bytes=%u\n", internal ? "internal" : "PSRAM",unsigned(sizeof(JPEGDEC)));
    return true;
}
bool PreviewDecoder::prepareNative(bool cameraCrop) {
#if defined(ORIGAMI_REDUCED_VIDEO_DECODE_TRIAL)
    // Compare the pinned JPEGDEC's reduced-IDCT path using the same output
    // surface/painter. The recording JPEG bytes are never changed.
    const bool prepared=prepare();
    reducedVideo_=prepared;
    return prepared;
#endif
    close();
    if (!cameraCrop) {
        videoPixels_=static_cast<uint16_t*>(heap_caps_aligned_calloc(16,1,640u*16u*2u,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if (!videoPixels_) return false;
        videoPixelBytes_=640u*16u*2u;
    }
    jpeg_dec_config_t config{};
    config.output_type=JPEG_PIXEL_FORMAT_RGB565_BE;
    config.scale.width=0;config.scale.height=0;
    config.rotate=cameraCrop ? JPEG_ROTATE_270D : JPEG_ROTATE_0D;
    config.block_enable=!cameraCrop;
    if (cameraCrop) {config.scale.width=320;config.scale.height=240;config.clipper.width=280;config.clipper.height=240;}
    if (jpeg_dec_open(&config,&videoDecoder_)!=JPEG_ERR_OK || !videoDecoder_) {
        close();return false;
    }
    cameraCrop_=cameraCrop;
    return true;
}
PreviewDecoder::Result PreviewDecoder::decodeVideo(const uint8_t* data,size_t length,
        uint16_t* destination,ShouldAbort abort,void* user,bool fit) {
    if (!videoDecoder_ || (!cameraCrop_ && !videoPixels_) || !data || !destination || !length || length>INT32_MAX) return Result::Invalid;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    const uint32_t started=micros();
    uint32_t pixelUs=0;
#endif
    if (abort && abort(user)) return Result::Interrupted;
    if (!supportedVideoJpeg(data,length)) return Result::Invalid;
    jpeg_dec_io_t io{};jpeg_dec_header_info_t header{};
    io.inbuf=const_cast<uint8_t*>(data);io.inbuf_len=static_cast<int>(length);
    if (jpeg_dec_parse_header(videoDecoder_,&io,&header)!=JPEG_ERR_OK) return Result::Invalid;
    if (cameraCrop_) {
        // Native header reports transformed output dimensions, not JPEG SOF dimensions.
        if (header.width!=240 || header.height!=280) return Result::Invalid;
        if (fit || (reinterpret_cast<uintptr_t>(destination)&15u)) return Result::Invalid;
        int bytes=0,blocks=0;
        if (jpeg_dec_get_outbuf_len(videoDecoder_,&bytes)!=JPEG_ERR_OK || bytes!=int(kPixels*2) ||
            jpeg_dec_get_process_count(videoDecoder_,&blocks)!=JPEG_ERR_OK || blocks!=1) return Result::Invalid;
        if (abort && abort(user)) return Result::Interrupted;
        io.outbuf=reinterpret_cast<uint8_t*>(destination);
        if (jpeg_dec_process(videoDecoder_,&io)!=JPEG_ERR_OK || io.out_size!=bytes) return Result::Invalid;
        if (abort && abort(user)) return Result::Interrupted;
        return Result::Decoded;
    }
    if (!core::avi::supportedDimensions(header.width,header.height) || (header.width==1280 && !fit)) return Result::Invalid;
    int bytes=0,blocks=0;
    if (jpeg_dec_get_outbuf_len(videoDecoder_,&bytes)!=JPEG_ERR_OK || bytes<=0 || bytes>header.width*16*2 ||
        bytes%(header.width*2)!=0 || jpeg_dec_get_process_count(videoDecoder_,&blocks)!=JPEG_ERR_OK ||
        blocks<=0 || blocks>header.height/8 || bytes*blocks!=header.width*header.height*2) return Result::Invalid;
    if (size_t(bytes)>videoPixelBytes_) {
        auto* expanded=static_cast<uint16_t*>(heap_caps_aligned_calloc(16,1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if (!expanded) return Result::Invalid;
        heap_caps_free(videoPixels_);videoPixels_=expanded;videoPixelBytes_=bytes;
    }
    io.outbuf=reinterpret_cast<uint8_t*>(videoPixels_);
    // Successful decoding replaces every image pixel; only fitted output has bars.
    const int outputHeight=fit ? 280*header.height/header.width : 240;
    const int top=fit ? (240-outputHeight)/2 : 0;
    for (int x=0;fit && x<280;++x) {
        auto* column=destination+x*240;
        memset(column,0,top*sizeof(uint16_t));
        memset(column+top+outputHeight,0,(240-top-outputHeight)*sizeof(uint16_t));
    }
    // Compute the fitted coordinates once per frame/row, rather than repeating
    // division and stride multiplication for each destination pixel. Bounded
    // UI-stack scratch; the output geometry and native byte order are unchanged.
    uint16_t sourceX[280],rowOffsets[240];
    for (int x=0;x<280;++x) sourceX[x]=fit ? x*header.width/280 : (x+20)*header.width/320;
    int sourceRow=0;
    for (int block=0;block<blocks;++block) {
        if (abort && abort(user)) return Result::Interrupted;
        if (jpeg_dec_process(videoDecoder_,&io)!=JPEG_ERR_OK || io.out_size!=bytes) return Result::Invalid;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
        const uint32_t pixelStarted=micros();
#endif
        const int rows=bytes/(header.width*2);
        const int y0=(sourceRow*outputHeight+header.height-1)/header.height;
        const int y1=((sourceRow+rows)*outputHeight+header.height-1)/header.height;
        for (int y=y0;y<y1;++y) rowOffsets[y]=(y*header.height/outputHeight-sourceRow)*header.width;
        for (int x=0;x<280;++x) {
            auto* dst=destination+(279-x)*240+top;
            const int sx=sourceX[x];
            for (int y=y0;y<y1;++y) dst[y]=videoPixels_[rowOffsets[y]+sx];
        }
        sourceRow+=rows;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
        pixelUs+=uint32_t(micros()-pixelStarted);
#endif
    }
    if (abort && abort(user)) return Result::Interrupted;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    ++profileCount_;profileTotalUs_+=uint32_t(micros()-started);
    profileWriteUs_+=pixelUs;profileBlocks_+=blocks;profileBytes_+=length;
    if (profileCount_==16) {
        Serial.printf("[video-native-decode] frames=%lu total_us=%lu pixel_us=%lu blocks=%lu jpeg_bytes=%lu\n",
            (unsigned long)profileCount_,(unsigned long)profileTotalUs_,
            (unsigned long)profileWriteUs_,(unsigned long)profileBlocks_,(unsigned long)profileBytes_);
        profileCount_=profileTotalUs_=profileWriteUs_=profileBlocks_=profileBytes_=0;
    }
#endif
    return Result::Decoded;
}
PreviewDecoder::Result PreviewDecoder::decode(const uint8_t* data, size_t length,
        uint16_t* destination, ShouldAbort shouldAbort, void* user, bool fit) {
    if (videoDecoder_) return decodeVideo(data,length,destination,shouldAbort,user,fit);
    if (!decoder_ || !destination || !data || length > INT32_MAX) return Result::Invalid;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    const uint32_t started=micros();
#endif
    if (reducedVideo_ && !supportedVideoJpeg(data,length)) return Result::Invalid;
    Decode context{destination, shouldAbort, user, false, fit, 240, 210, 15, {}, {}};
    JPEGDEC& jpeg = *decoder_;
    if (!jpeg.openRAM(const_cast<uint8_t*>(data), (int)length, writeBlock)) return Result::Invalid;
    const bool vga = jpeg.getWidth() == 640 && jpeg.getHeight() == 480;
    const bool hd = jpeg.getWidth() == 1280 && jpeg.getHeight() == 720;
    if ((!vga && !hd && !(jpeg.getWidth() == 320 && jpeg.getHeight() == 240)) || (hd && !fit)) {
        jpeg.close();
        return Result::Invalid;
    }
    if (hd) { context.sourceHeight=180;context.outputHeight=157;context.top=41; }
    for (int x = 0; x < 280; ++x) context.sourceX[x] = fit ? x*320/280 : x+20;
    for (int y = 0; y < 240; ++y) context.sourceY[y] = fit ? y*context.sourceHeight/context.outputHeight : y;
    if (fit) memset(destination,0,kPixels*sizeof(uint16_t));
    jpeg.setUserPointer(&context);
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    const bool decoded = jpeg.decode(0, 0, hd ? JPEG_SCALE_QUARTER : vga ? JPEG_SCALE_HALF : 0) != 0;
    jpeg.close();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    // Per-instance, owning-task counters. Batches keep serial output bounded.
    // Counts reset on close and include successful fitted video decodes.
    if (fit && decoded && !context.interrupted) {
        auto& count=profileCount_;auto& totalUs=profileTotalUs_;auto& writeUs=profileWriteUs_;
        auto& blocks=profileBlocks_;auto& bytes=profileBytes_;
        ++count;totalUs+=uint32_t(micros()-started);writeUs+=context.writeUs;
        blocks+=context.blocks;bytes+=length;
        if (count==16) {
            Serial.printf("[video-decode] frames=%lu total_us=%lu pixel_us=%lu blocks=%lu jpeg_bytes=%lu\n",
                (unsigned long)count,(unsigned long)totalUs,(unsigned long)writeUs,
                (unsigned long)blocks,(unsigned long)bytes);
            count=totalUs=writeUs=blocks=bytes=0;
        }
    }
#endif
    if (context.interrupted) return Result::Interrupted;
    return decoded ? Result::Decoded : Result::Invalid;
}
} }
