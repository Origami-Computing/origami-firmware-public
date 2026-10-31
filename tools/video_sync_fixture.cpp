// Host-only fixture packer using the production AVI codec. No device access.
#include "core/mjpeg_avi.h"
#include <vector>
#include <fstream>
#include <iterator>
#include <cassert>
#include <algorithm>
using Bytes=std::vector<uint8_t>;
Bytes read(const char* path){std::ifstream f(path,std::ios::binary);return Bytes((std::istreambuf_iterator<char>(f)),{});}
int main(int argc,char** argv){
 assert(argc==5);auto dark=read(argv[1]),light=read(argv[2]),pcm=read(argv[3]);
 assert(!dark.empty()&&!light.empty()&&pcm.size()==384000);
 Bytes movie,index;
 auto append=[&](const uint8_t* p,uint32_t n,bool audio){
  size_t at=movie.size(),idx=index.size();movie.resize(at+core::avi::chunkBytes(n));index.resize(idx+16);
  core::avi::chunk(movie.data()+at,n,audio);memcpy(movie.data()+at+8,p,n);
  core::avi::index(index.data()+idx,at+4,n,audio);
 };
 for(unsigned frame=0;frame<100;++frame){
  const auto& jpeg=frame>=25&&frame%25<5?light:dark;
  append(jpeg.data(),jpeg.size(),false);append(pcm.data()+frame*3840,3840,true);
 }
 Bytes out(324+movie.size()+8+index.size());
 core::avi::audioHeader(out.data(),640,480,100,4000,movie.size(),std::max(dark.size(),light.size()),192000,index.size()/16);
 memcpy(out.data()+324,movie.data(),movie.size());auto* tail=out.data()+324+movie.size();
 memcpy(tail,"idx1",4);core::avi::u32(tail+4,index.size());memcpy(tail+8,index.data(),index.size());
 core::avi::Info info;assert(core::avi::parse(out.data(),out.size(),info,324));
 std::ofstream(argv[4],std::ios::binary).write(reinterpret_cast<const char*>(out.data()),out.size());
}
