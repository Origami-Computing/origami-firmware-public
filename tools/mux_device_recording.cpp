// Host-only migration of an explicitly identified silent AVI + raw PCM pair.
#include "core/mjpeg_avi.h"
#include <vector>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <algorithm>
using Bytes=std::vector<uint8_t>;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
Bytes read(const char* path){
 std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"Cannot open source");
 auto size=f.tellg();require(size>0&&size<=256*1024*1024,"Source size outside limits");
 Bytes bytes(static_cast<size_t>(size));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
 require(bool(f),"Source read failed");return bytes;
}
int main(int argc,char** argv){try{
 require(argc==4,"Expected AVI, raw mono PCM16, output AVI");
 require(!std::filesystem::exists(argv[3]),"Output already exists");
 auto video=read(argv[1]),pcm=read(argv[2]);core::avi::Info info;
 require(video.size()>=224&&core::avi::parse(video.data(),video.size(),info)&&!info.audioSamples,"Unsupported source AVI");
 require(!(pcm.size()%2)&&pcm.size()/2<=core::avi::kAudioRate*300,"PCM size outside limits");
 std::vector<core::avi::Chunk> frames;uint32_t at=info.headerBytes;
 while(at<info.movieEnd){core::avi::Chunk chunk;
  require(core::avi::parseChunk(video.data()+at,at,info,chunk)&&!chunk.audio,"Invalid source chunk");
  require(video[chunk.dataOffset]==0xff&&video[chunk.dataOffset+1]==0xd8,"Invalid source JPEG");
  frames.push_back(chunk);at=chunk.nextOffset;
 }
 require(frames.size()==info.frames,"Source frame count mismatch");
 Bytes movie,index;uint32_t largest=0;
 auto append=[&](const uint8_t* p,uint32_t bytes,bool audio){
  size_t start=movie.size(),idx=index.size();
  require(start+core::avi::chunkBytes(bytes)+index.size()+324<256u*1024*1024,"Combined file exceeds size limit");
  movie.resize(start+core::avi::chunkBytes(bytes));index.resize(idx+16);
  core::avi::chunk(movie.data()+start,bytes,audio);memcpy(movie.data()+start+8,p,bytes);
  core::avi::index(index.data()+idx,start+4,bytes,audio);
  if(!audio)largest=std::max(largest,bytes);
 };
 uint32_t vi=0,samples=0,total=pcm.size()/2;
 while(vi<frames.size()||samples<total){
  if(vi<frames.size()&&(samples==total||uint64_t(vi)*info.intervalUs<=uint64_t(samples)*1000000/core::avi::kAudioRate)){
   auto& f=frames[vi++];append(video.data()+f.dataOffset,f.bytes,false);
  }else{uint32_t count=std::min(core::avi::kAudioChunkBytes/2,total-samples);append(pcm.data()+samples*2,count*2,true);samples+=count;}
 }
 const uint32_t duration=uint64_t(core::avi::read32(video.data()+128))*info.frames*1000/core::avi::read32(video.data()+132);
 Bytes out(324+movie.size()+8+index.size());
 core::avi::audioHeader(out.data(),info.width,info.height,info.frames,duration,movie.size(),largest,total,index.size()/16);
 memcpy(out.data()+324,movie.data(),movie.size());auto* tail=out.data()+324+movie.size();
 memcpy(tail,"idx1",4);core::avi::u32(tail+4,index.size());memcpy(tail+8,index.data(),index.size());
 require(core::avi::parse(out.data(),out.size(),info,324),"Combined header rejected");
 std::ofstream file(argv[3],std::ios::binary);file.write(reinterpret_cast<const char*>(out.data()),out.size());
 require(bool(file),"Output write failed");
 std::cout<<"Combined "<<info.frames<<" original JPEG frames and "<<total<<" PCM samples; originals unchanged\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
