#include "services/video_thumbnail.h"
#include "core/mjpeg_avi.h"
#include "core/memory.h"
#include "hal/hal_sd.h"
#include <algorithm>
namespace services { namespace video {
void Thumbnail::clear(){void* p=data_;core::memory::freePsram(p);data_=nullptr;bytes_=durationMs_=0;}
bool Thumbnail::load(const char* path){
    clear();
    hal::sd::StreamReader file;
    if(!hal::sd::openStream(path,&file))return false;
    auto exact=[&](uint8_t* out,size_t bytes){
        while(bytes){size_t n=hal::sd::readStream(&file,out,std::min(bytes,size_t(8192)));if(!n)return false;out+=n;bytes-=n;}return true;
    };
    uint8_t header[core::avi::kAudioHeaderBytes];core::avi::Info info;
    bool ok=file.size<=UINT32_MAX && exact(header,core::avi::kHeaderBytes);
    size_t headerBytes=core::avi::kHeaderBytes;
    if(ok && core::avi::read32(header+56)==2){ok=exact(header+headerBytes,sizeof(header)-headerBytes);headerBytes=sizeof(header);}
    ok=ok && core::avi::parse(header,uint32_t(file.size),info,headerBytes);
    // Bounded scan: only chunk headers are read before the first JPEG. Skip PCM.
    for(unsigned chunks=0;ok && !data_ && chunks<128;++chunks){
        uint8_t tag[8];core::avi::Chunk chunk;
        const uint32_t at=file.position;
        ok=exact(tag,8) && core::avi::parseChunk(tag,at,info,chunk);
        if(!ok)break;
        if(!chunk.audio){
            if(!chunk.bytes){ok=false;break;}
            data_=static_cast<uint8_t*>(core::memory::allocPsram("video.thumbnail",chunk.bytes));
            ok=data_ && exact(data_,chunk.bytes);
            if(ok)bytes_=chunk.bytes;
        }else ok=hal::sd::seekStream(&file,chunk.nextOffset);
    }
    hal::sd::closeStream(&file);
    if(!ok || !bytes_){clear();return false;}
    durationMs_=uint32_t(std::max(uint64_t(info.frames)*info.intervalUs/1000,uint64_t(info.audioSamples)*1000/core::avi::kAudioRate));
    return true;
}
} }
