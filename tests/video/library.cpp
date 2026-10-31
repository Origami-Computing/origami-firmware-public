#include "services/video_library.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"
#include <vector>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <iostream>
using Library=services::video::Library;
using State=Library::State;
std::thread worker;
bool mounted=true,admission=true,failAlloc=false,failSpawn=false,failOpen=false,failRead=false;
int leases=0,allocations=0,opened=0,radios=0;
std::atomic<unsigned> jpegReads{0};
std::atomic<bool> slowIndex{false},indexRead{false};
size_t failBytes=0;
std::vector<uint8_t> file;
void* heap_caps_malloc(size_t bytes,int){if(failAlloc || bytes==failBytes)return nullptr;++allocations;return malloc(bytes);}
void heap_caps_free(void* p){if(p){--allocations;free(p);}}
namespace hal {namespace tasks {
void* spawn(Id,void(*fn)(void*),void* p){if(worker.joinable())worker.join();if(failSpawn)return nullptr;worker=std::thread(fn,p);return reinterpret_cast<void*>(1);}
void recordCurrentStack(Id){}void deleteCurrent(Id){}
}}
namespace hal {namespace sd {
bool mounted(){return ::mounted;}
int listFiles(const char* dir,const char* ext,char* out,int count,int cap){assert(!strcmp(dir,"/videos")&&!strcmp(ext,".avi")&&count>0&&cap>16);strcpy(out,"/videos/test.avi");return 1;}
bool openStream(const char*,StreamReader* r){if(failOpen)return false;r->handle=&file;r->size=file.size();r->position=0;++opened;return true;}
size_t readStream(StreamReader* r,uint8_t* dst,size_t len){if(failRead)return 0;
if(slowIndex.load() && r->position>224) {indexRead=true;std::this_thread::sleep_for(std::chrono::milliseconds(20));}
if(len==5)++jpegReads;len=std::min(len,r->size-r->position);memcpy(dst,file.data()+r->position,len);r->position+=len;return len;}
bool seekStream(StreamReader* r,size_t at){if(failRead||at>r->size)return false;r->position=at;return true;}
void closeStream(StreamReader* r){if(r->handle){--opened;r->handle=nullptr;}}
}}
void join(){if(worker.joinable())worker.join();}
void makeFile(unsigned frames=5, unsigned durationMs=1000){
 const unsigned frameSize=5,movie=frames*core::avi::chunkBytes(frameSize);
 file.resize(224+movie+8+frames*16);
 core::avi::header(file.data(),640,480,frames,durationMs,movie,frameSize);
 for(unsigned i=0;i<frames;++i){auto* p=file.data()+224+i*14;core::avi::chunk(p,5);uint8_t jpeg[]={0xff,0xd8,uint8_t(i),0xff,0xd9};memcpy(p+8,jpeg,5);p[13]=0;core::avi::index(file.data()+224+movie+8+i*16,4+i*14,5);}
 memcpy(file.data()+224+movie,"idx1",4);core::avi::u32(file.data()+224+movie+4,frames*16);
}
void makeAudioFile(uint32_t audioBytes=4,unsigned frames=5){
 makeFile(frames);
 const auto original=file;const uint32_t stride=14+8+audioBytes,movie=frames*stride,entries=frames*2;
 file.assign(324+movie+8+entries*16,0);
 core::avi::audioHeader(file.data(),640,480,frames,1000,movie,5,frames*audioBytes/2,entries);
 for(unsigned i=0;i<frames;++i) {
  memcpy(file.data()+324+i*stride,original.data()+224+i*14,14);
  core::avi::chunk(file.data()+324+i*stride+14,audioBytes,true);
  core::avi::index(file.data()+324+movie+8+i*32,4+i*stride,5);
  core::avi::index(file.data()+324+movie+8+i*32+16,4+i*stride+14,audioBytes,true);
 }
 memcpy(file.data()+324+movie,"idx1",4);core::avi::u32(file.data()+324+movie+4,entries*16);
}
void waitState(Library& library,State wanted){
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
 while(library.state()!=wanted&&!(wanted==State::Ready&&library.state()==State::End)&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 assert(library.state()==wanted||(wanted==State::Ready&&library.state()==State::End));
}
void consume(Library& library,bool holds=false,unsigned frames=5){
 for(unsigned i=0;i<frames;++i){
  Library::Frame frame;auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(!library.take(frame)&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(frame.data&&frame.length==5&&frame.number==i&&frame.data[2]==(holds?0:i));
  assert(frame.imageNumber==(holds?0:i));
  Library::Frame second;assert(!library.take(second));library.release();
 }
}
int main(){
 makeFile();core::avi::Info info;
 assert(core::avi::parse(file.data(),file.size(),info)&&info.frames==5&&info.intervalUs==200000);
 for(auto at:{4,16,28,48,56,64,68,92,104,128,132,140,164,168,172,176,180,184,186,188,212,216,220}){
  auto broken=file;memset(broken.data()+at,0,4);assert(!core::avi::parse(broken.data(),broken.size(),info));
 }
 assert(!core::avi::parse(file.data(),223,info));
 auto broken=file;core::avi::u32(broken.data()+216,0xffffffff);assert(!core::avi::parse(broken.data(),broken.size(),info));
 // Reader accepts the higher-rate five-minute bound, rejects one beyond it.
 makeFile(core::avi::kMaxFrames,300000);
 assert(core::avi::parse(file.data(),file.size(),info));
 makeFile(core::avi::kMaxFrames+1,300000);
 assert(!core::avi::parse(file.data(),file.size(),info));
 makeFile();
 Library library;
 mounted=false;assert(!library.open());mounted=true;
 admission=false;assert(!library.open());admission=true;assert(!leases);
 admission=false;assert(!library.open(true) && library.opening() && !leases);
 assert(!library.open(true) && library.opening() && !leases);
 library.close();assert(!library.opening() && !leases);
 assert(!library.open(true) && library.opening());
 admission=true;assert(library.open(true) && !library.opening() && leases==1);
 library.close();assert(!leases);
 assert(library.open(false,"/videos/selected-long-gallery-file.avi"));
 assert(library.count()==1&&!strcmp(library.path(0),"/videos/selected-long-gallery-file.avi"));
 library.close();assert(!leases&&!allocations);
 assert(!library.open(false,"/photos/wrong.jpg")&&!leases&&!allocations);
 failAlloc=true;assert(!library.open());failAlloc=false;assert(!leases&&!allocations);
 assert(library.open()&&library.count()==1&&leases==1);assert(!strcmp(library.path(0),"/videos/test.avi"));assert(!*library.path(1));
 assert(!library.select(1));
 failAlloc=true;assert(!library.select(0));failAlloc=false;
 failSpawn=true;assert(!library.select(0));failSpawn=false;
 assert(library.select(0));waitState(library,State::Ready);
 // Let prefetch fill: bounded queue must preserve the first frame while paused.
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 consume(library);waitState(library,State::End);library.stop();join();assert(!opened&&leases==1);
 // Select the newest due image without consuming a future frame.
 makeFile(17);assert(library.select(0));waitState(library,State::Ready);
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 Library::Frame due;assert(library.take(due,1)&&due.number==1&&due.data[2]==1);
 // Producer may refill discarded slots but cannot overwrite the borrowed one.
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 Library::Frame second;assert(!library.take(second,4)&&due.data[2]==1);
 library.release();assert(!library.take(due,1));
 assert(library.take(due,2)&&due.number==2&&due.data[2]==2);library.release();
 library.stop();join();
 // Jump over expired payloads after the initial paused prefetch. The target
 // remains a real logical frame, and the last frame remains consumable.
 makeFile(100);jpegReads=0;assert(library.select(0));waitState(library,State::Ready);
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 assert(library.take(due,90)&&due.number==5);library.release();
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 assert(library.take(due,90)&&due.number==90&&due.data[2]==90);library.release();
 assert(!library.take(due,90));
 {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  do {
   if(library.take(due,99))library.release();
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while(due.number!=99 && std::chrono::steady_clock::now()<deadline);
  assert(due.number==99 && due.data[2]==99);
 }
 library.stop();join();assert(jpegReads<=18);
 // Worker-side index allocation failure must also unwind the session.
 makeFile();failBytes=5*sizeof(core::avi::IndexedFrame);
 assert(library.select(0));waitState(library,State::Failed);library.stop();join();
 failBytes=0;assert(allocations==1 && !opened && leases==1);
 // Invalid index offsets, flags, tags, sizes and totals fail before publishing.
 for(unsigned field:{0u,4u,8u,12u}) {
  makeFile();const size_t first=224+5*14+8;
  core::avi::u32(file.data()+first+field,0xffffffff);
  assert(library.select(0));waitState(library,State::Failed);
  assert(!library.take(due));library.stop();join();
 }
 // Maximum PCM payloads are bypassed without corrupting JPEG order.
 makeAudioFile(4096);assert(library.select(0));waitState(library,State::Ready);
 consume(library);waitState(library,State::End);library.stop();join();
 // Interleaved audio is skipped by the video reader without losing JPEG order.
 makeAudioFile();assert(library.select(0));waitState(library,State::Ready);
 assert(library.startAudio());library.pauseAudio();assert(library.startAudio());
 consume(library);waitState(library,State::End);library.stop();join();assert(!opened);
 // Hold chunks survive several queue wraps and preserve the image when UI skips frames.
 makeAudioFile(4,17);
 {
  auto original=file;std::vector<uint8_t> movie,index;
  core::avi::Info info;assert(core::avi::parse(original.data(),original.size(),info,324));
  unsigned images=0;
  for(uint32_t at=324;at<info.movieEnd;) {
   core::avi::Chunk chunk;assert(core::avi::parseChunk(original.data()+at,at,info,chunk));
   const unsigned bytes=!chunk.audio && images++ ? 0 : chunk.bytes;
   const size_t pos=movie.size(),entry=index.size();movie.resize(pos+core::avi::chunkBytes(bytes));index.resize(entry+16);
   core::avi::chunk(movie.data()+pos,bytes,chunk.audio);
   memcpy(movie.data()+pos+8,original.data()+chunk.dataOffset,bytes);
   core::avi::index(index.data()+entry,pos+4,bytes,chunk.audio);at=chunk.nextOffset;
  }
  file.resize(324+movie.size()+8+index.size());
  core::avi::audioHeader(file.data(),640,480,info.frames,1000,movie.size(),5,info.audioSamples,info.indexEntries);
  memcpy(file.data()+324,movie.data(),movie.size());
  memcpy(file.data()+324+movie.size(),"idx1",4);core::avi::u32(file.data()+328+movie.size(),index.size());
  memcpy(file.data()+332+movie.size(),index.data(),index.size());
 }
 assert(library.select(0));consume(library,true,17);waitState(library,State::End);library.stop();join();
 assert(library.select(0));waitState(library,State::Ready);
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 assert(library.take(due,16));library.release();
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 assert(library.take(due,16)&&due.number==16&&due.imageNumber==0&&due.data[2]==0);
 library.release();waitState(library,State::End);library.stop();join();
 core::avi::u32(file.data()+328,0); // A leading hold has no image to repeat.
 assert(library.select(0));waitState(library,State::Failed);library.stop();join();
 // Inconsistent audio totals fail instead of accepting a truncated recording.
 makeAudioFile();core::avi::u32(file.data()+264,11);
 assert(library.select(0));waitState(library,State::Failed);library.stop();join();
 // Cancel while the index is still loading; cleanup waits for the active read.
 makeFile(core::avi::kMaxFrames,300000);slowIndex=true;indexRead=false;
 assert(library.select(0));
 {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(!indexRead.load() && std::chrono::steady_clock::now()<deadline)
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(indexRead.load());
 }
 library.stop();join();slowIndex=false;
 assert(library.state()==State::Idle && !opened && allocations==1);
 // Cancel a full paused queue and repeatedly restart without freeing live buffers.
 makeFile(17);
 for(int i=0;i<10;++i){assert(library.select(0));waitState(library,State::Ready);std::this_thread::sleep_for(std::chrono::milliseconds(10));library.stop();join();assert(!opened);}
 failOpen=true;assert(library.select(0));waitState(library,State::Failed);library.stop();join();failOpen=false;
 failRead=true;assert(library.select(0));waitState(library,State::Failed);library.stop();join();failRead=false;
 // Selected chunk claiming bytes outside movi fails before its payload read.
 makeFile();core::avi::u32(file.data()+228,0xffffffff);assert(library.select(0));waitState(library,State::Failed);library.stop();join();
 makeFile();file[224+5*14]='x';assert(library.select(0));waitState(library,State::Failed);library.stop();join();
 library.close();library.close();assert(!leases&&!allocations&&!opened&&!radios);
 std::cout<<"PASS AVI bounds/layout, ordered prefetch, paused queue, cancel/restart, failed admission/allocation/spawn/read and cleanup\n";
}
