#include "core/mjpeg_avi.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>
#include <random>
#include <iostream>
using Bytes=std::vector<uint8_t>;
int main(int argc,char** argv) {
 assert(argc==4 || argc==6);
 const unsigned width=argc==6?std::stoul(argv[4]):640, height=argc==6?std::stoul(argv[5]):480;
 std::ifstream input(argv[1],std::ios::binary);
 Bytes jpeg((std::istreambuf_iterator<char>(input)),{});assert(!jpeg.empty());
 Bytes movie,index,pcm;
 auto append=[&](const uint8_t* data,uint32_t bytes,bool audio) {
  const size_t at=movie.size(),entry=index.size();
  movie.resize(at+core::avi::chunkBytes(bytes));index.resize(entry+16);
  core::avi::chunk(movie.data()+at,bytes,audio);memcpy(movie.data()+at+8,data,bytes);
  core::avi::index(index.data()+entry,uint32_t(at+4),bytes,audio);
 };
 for(unsigned frame=0;frame<8;++frame) {
  append(jpeg.data(),frame%2 && frame!=7 ? 0 : jpeg.size(),false);
  for(unsigned at=0;at<4800;) {
   unsigned samples=std::min(2048u,4800-at);Bytes block(samples*2);
   for(unsigned i=0;i<samples;++i) {
    uint16_t value=uint16_t((frame*4800+at+i)*37);
    block[i*2]=uint8_t(value);block[i*2+1]=uint8_t(value>>8);
   }
   append(block.data(),block.size(),true);pcm.insert(pcm.end(),block.begin(),block.end());at+=samples;
  }
 }
 Bytes avi(core::avi::kAudioHeaderBytes+movie.size()+8+index.size());
 core::avi::audioHeader(avi.data(),width,height,8,800,movie.size(),jpeg.size(),38400,index.size()/16);
 memcpy(avi.data()+324,movie.data(),movie.size());
 auto* tail=avi.data()+324+movie.size();memcpy(tail,"idx1",4);core::avi::u32(tail+4,index.size());memcpy(tail+8,index.data(),index.size());
 core::avi::Info info;
 assert(!core::avi::parse(avi.data(),avi.size(),info));
 assert(!core::avi::parse(avi.data(),avi.size(),info,323));
 assert(core::avi::parse(avi.data(),avi.size(),info,324));
 assert(info.frames==8&&info.intervalUs==100000&&info.audioSamples==38400&&info.headerBytes==324&&info.indexEntries==32);
 for(unsigned offset:{16,56,212,216,220,224,228,232,252,256,264,276,288,292,296,300,304,308,312,316,320}) {
  auto bad=avi;memset(bad.data()+offset,0,4);assert(!core::avi::parse(bad.data(),bad.size(),info,324));
 }
 // Walk real interleaved chunks using the shared bounded parser.
 uint32_t position=info.headerBytes, videos=0,samples=0;
 core::avi::Chunk chunk;
 while(position<info.movieEnd) {
  assert(core::avi::parseChunk(avi.data()+position,position,info,chunk));
  if(chunk.audio)samples+=chunk.bytes/2;else ++videos;
  position=chunk.nextOffset;
 }
 assert(videos==8&&samples==38400);
 uint8_t invalid[8];core::avi::chunk(invalid,3,true);
 assert(!core::avi::parseChunk(invalid,info.headerBytes,info,chunk));
 core::avi::chunk(invalid,0xffffffff,false);
 assert(!core::avi::parseChunk(invalid,info.headerBytes,info,chunk));
 assert(!core::avi::parseChunk(invalid,info.movieEnd-4,info,chunk));
 assert(!core::avi::parseChunk(invalid,info.headerBytes-1,info,chunk));
 // A two-stream marker in an actually short buffer must not read its missing header.
 Bytes shortHeader(avi.begin(),avi.begin()+224);
 assert(!core::avi::parse(shortHeader.data(),avi.size(),info));
 std::mt19937 random(42);
 for(int trial=0;trial<5000;++trial) {
  auto bad=shortHeader;for(int n=0;n<8;++n)bad[random()%bad.size()]=uint8_t(random());
  core::avi::parse(bad.data(),uint32_t(random()),info,bad.size());
 }
 std::ofstream(argv[2],std::ios::binary).write(reinterpret_cast<const char*>(avi.data()),avi.size());
 std::ofstream(argv[3],std::ios::binary).write(reinterpret_cast<const char*>(pcm.data()),pcm.size());
 std::cout<<"PASS bounded MJPEG/PCM header, stream fields, incomplete headers and mutated input under sanitizers\n";
}
