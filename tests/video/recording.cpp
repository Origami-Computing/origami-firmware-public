#include "services/video_recording.h"
#include "services/camera_session.h"
#include <Arduino.h>
#include "core/mjpeg_avi.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <iostream>
using Recording = services::camera::VideoRecording;
using State = Recording::State;
std::thread worker,micWorker,cameraWorker;
std::atomic<bool> cameraLoan{false},cameraReading{false},holdCameraRead{false},cameraBlocked{false},failCameraRead{false};
std::atomic<unsigned> cameraReads{0};
bool failCameraSpawn=false,failCameraAlloc=false,failCameraLoan=false;
std::vector<uint8_t> cameraInput;
uint16_t cameraWidth=640,cameraHeight=480;
namespace services {namespace camera {
bool Session::FrameReader::acquire(Session& s){if(failCameraLoan||cameraLoan||!s.active)return false;cameraLoan=true;return true;}
bool Session::FrameReader::read(hal::camera::CameraFrame* frame){
 assert(cameraLoan&&!held);cameraReading=true;
 while(holdCameraRead){cameraBlocked=true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}cameraBlocked=false;
 std::this_thread::sleep_for(std::chrono::milliseconds(4));
 if(failCameraRead){cameraReading=false;return false;}
 bytes=cameraInput;bytes.insert(bytes.begin()+2,{0xff,0xfe,0,3,uint8_t(++cameraReads)});
 *frame={bytes.data(),bytes.size(),cameraWidth,cameraHeight,4,uint64_t(millis())*1000};held=true;cameraReading=false;return true;
}
void Session::FrameReader::returnFrame(){if(held){std::fill(bytes.begin(),bytes.end(),0xcc);held=false;}}
void Session::FrameReader::close(){assert(!cameraReading);returnFrame();cameraLoan=false;}
}}

bool failMicAdmission=false,failMicInit=false,failMicSpawn=false;
int micLeases=0,micOwners=0;
std::atomic<bool> failMicRead{false},fastMic{false};
std::atomic<uint32_t> micSamples{0};
uint32_t micLimit=9600;
namespace hal {namespace audio {
bool initMic(bool streaming){assert(streaming);++micOwners;micSamples=0;return !failMicInit;}
void endMicSession(){--micOwners;}
int readMic(void* raw,size_t bytes,size_t* got,unsigned){
 if(!fastMic)std::this_thread::sleep_for(std::chrono::milliseconds(5));
 if(failMicRead)return -1;
 unsigned start=micSamples.load(),count=std::min(unsigned(bytes/4),micLimit-start);
 auto* out=static_cast<int32_t*>(raw);
 for(unsigned i=0;i<count;++i)out[i]=(int32_t((start+i)%1000)-500)*65536;
 *got=count*4;micSamples=start+count;return 0;
}
}}
std::map<std::string,std::vector<uint8_t>> files;
std::atomic<bool> blockWrites{false}, writing{false};
bool mounted=true, failAlloc=false, failSpawn=false, failOpen=false, failSeek=false, failRename=false, failClose=false;
int writes=0, failWrite=0, allocations=0, closes=0;
unsigned videoWriteDelayMs=0;bool videoPayload=false;
void* heap_caps_malloc(size_t bytes,int) {if(failAlloc||(failCameraAlloc&&bytes==3*core::avi::kJpegFrameBytes))return nullptr;++allocations;return malloc(bytes);}
void heap_caps_free(void* p){if(p){--allocations;free(p);}}
uint32_t esp_random(){static uint32_t n=0;return ++n;}
namespace hal {namespace tasks {
void* spawn(Id id,void(*fn)(void*),void* p){
 auto& target=id==Id::VideoWrite?worker:id==Id::VideoMic?micWorker:cameraWorker;
 if(target.joinable())target.join();
 if(id==Id::VideoWrite?failSpawn:id==Id::VideoMic?failMicSpawn:failCameraSpawn)return nullptr;
 target=std::thread(fn,p);return reinterpret_cast<void*>(1);
}
void deleteCurrent(Id){}
void recordCurrentStack(Id){}
}}
struct Output {std::string path;size_t at=0;};
namespace hal {namespace sd {
bool mounted(){return ::mounted;}
bool ensureDir(const char*){return ::mounted;}
bool exists(const char* p){return files.count(p);}
bool openWriter(const char* p,StreamWriter* out){if(failOpen)return false;out->handle=new Output{p};files[p].clear();return true;}
bool writeStream(StreamWriter* out,const uint8_t* bytes,size_t n){
 writing=true;while(blockWrites)std::this_thread::sleep_for(std::chrono::milliseconds(1));writing=false;
 if(n==8 && !memcmp(bytes,"00dc",4))videoPayload=core::avi::read32(bytes+4)!=0;
 else if(videoPayload){
  videoPayload=false;
  std::this_thread::sleep_for(std::chrono::milliseconds(videoWriteDelayMs));
 }
 if(++writes==failWrite)return false;
 auto& f=*static_cast<Output*>(out->handle);auto& data=files[f.path];
 if(data.size()<f.at+n)data.resize(f.at+n);memcpy(data.data()+f.at,bytes,n);f.at+=n;return true;
}
bool seekWriter(StreamWriter* out,uint32_t n){if(failSeek)return false;static_cast<Output*>(out->handle)->at=n;return true;}
bool closeWriter(StreamWriter* out){if(out->handle){delete static_cast<Output*>(out->handle);out->handle=nullptr;++closes;}return !failClose;}
bool replaceFile(const char* from,const char* to){if(failRename)return false;files[to]=files.at(from);files.erase(from);return true;}
bool removeFile(const char* p){return files.erase(p);}
}}
void waitFor(Recording& r,State expected){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(r.state()!=expected&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 assert(r.state()==expected);
}
void join(){if(cameraWorker.joinable())cameraWorker.join();if(worker.joinable())worker.join();if(micWorker.joinable())micWorker.join();}
uint32_t u32(const std::vector<uint8_t>& b,size_t at){return uint32_t(b[at])|uint32_t(b[at+1])<<8|uint32_t(b[at+2])<<16|uint32_t(b[at+3])<<24;}
int main(int argc,char** argv){
 std::vector<uint8_t> jpeg{0xff,0xd8,0xff,0xc0,0,11,8,1,224,2,128,1,1,0x11,0,0xff,0xda,0,8,1,1,0,0,63,0,0x42,0xff,0xd9};
 std::ifstream input(argv[2],std::ios::binary);if(input)jpeg.assign(std::istreambuf_iterator<char>(input),{});
 auto marked=[&](uint8_t marker){auto image=jpeg;image.insert(image.begin()+2,{0xff,0xfe,0,3,marker});return image;};
 auto alternate=jpeg;
 if(argc>3){std::ifstream other(argv[3],std::ios::binary);if(other)alternate.assign(std::istreambuf_iterator<char>(other),{});}
 Recording r;
 {services::camera::Session camera;cameraInput=jpeg;
  for(int failure=0;failure<3;++failure){
   failCameraAlloc=failure==0;failCameraLoan=failure==1;failCameraSpawn=failure==2;
   assert(!r.beginCamera(camera,640,480,false));join();
   assert(!r.busy()&&!cameraLoan&&allocations==0&&r.state()==State::Failed);
  }
  failCameraAlloc=failCameraLoan=failCameraSpawn=false;
  assert(r.beginCamera(camera,640,480,false));waitFor(r,State::Recording);
  hal::camera::CameraFrame preview{};
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!r.readPreview(&preview)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(preview.buf&&preview.len==jpeg.size()+5);
  std::vector<uint8_t> held(preview.buf,preview.buf+preview.len);const auto reads=cameraReads.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  assert(cameraReads>reads+3&&!memcmp(preview.buf,held.data(),held.size()));
  assert(!r.readPreview(&preview)&&!r.submit(jpeg.data(),jpeg.size(),millis()));
  r.releasePreview();assert(r.readPreview(&preview));
  // Stop while a sensor read is outstanding. close must retain the loan until
  // that read returns, and may release an outstanding UI preview only afterward.
  holdCameraRead=true;
  while(!cameraBlocked)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::thread release([&]{std::this_thread::sleep_for(std::chrono::milliseconds(30));assert(cameraLoan);holdCameraRead=false;});
  r.close();release.join();join();
  assert(!cameraLoan&&!cameraReading&&allocations==0&&r.state()==State::Saved);
  assert(r.captureStats().unique>4&&r.submissionStats().accepted>4);
  assert(r.frames()==r.submissionStats().accepted);
  // Capture-only admission must not allocate or publish preview storage.
  failCameraAlloc=true;
  assert(r.beginCamera(camera,640,480,false,false));waitFor(r,State::Recording);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  assert(!r.readPreview(&preview));r.close();join();failCameraAlloc=false;
  assert(r.state()==State::Saved && r.captureStats().unique>0 && !allocations && !cameraLoan);
  // HD follows the same acquisition, microphone and acknowledged-close owners.
  std::ifstream hdInput(argv[4],std::ios::binary);
  cameraInput.assign(std::istreambuf_iterator<char>(hdInput),{});assert(!cameraInput.empty());
  cameraWidth=1280;cameraHeight=720;
  assert(r.beginCamera(camera,1280,720,true,false));waitFor(r,State::Recording);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));r.close();join();
  assert(r.state()==State::Saved && r.audioSamples()>0 && r.captureStats().unique>0);
  core::avi::Info hdInfo;auto hd=files[r.path()];
  assert(core::avi::parse(hd.data(),hd.size(),hdInfo,324) && hdInfo.width==1280 && hdInfo.height==720);
  std::ofstream(std::string(argv[1])+".hd.avi",std::ios::binary).write((char*)hd.data(),hd.size());
  assert(!allocations&&!cameraLoan&&!micOwners&&!micLeases);
  cameraInput=jpeg;cameraWidth=640;cameraHeight=480;
  // Camera failure stops the writer and never reports a successful clip.
  failCameraRead=true;assert(r.beginCamera(camera,640,480,false));
  waitFor(r,State::Failed);r.close();join();failCameraRead=false;
  assert(!cameraLoan&&!allocations&&!strcmp(r.error(),"Camera frame timeout"));
  // Re-entry with microphone ownership follows the same acknowledged teardown.
  assert(r.beginCamera(camera,640,480,true));waitFor(r,State::Recording);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  r.close();join();assert(r.state()==State::Saved&&r.audioSamples()>0);
  assert(!cameraLoan&&!allocations&&!micOwners&&!micLeases);
 }

 mounted=false;assert(!r.begin(640,480));assert(r.state()==State::Failed);mounted=true;
 assert(!r.begin(1280,721));
 failAlloc=true;assert(!r.begin(640,480));failAlloc=false;assert(!allocations);
 failSpawn=true;assert(!r.begin(640,480));failSpawn=false;assert(!allocations);
 failOpen=true;assert(r.begin(640,480));waitFor(r,State::Failed);r.close();join();failOpen=false;
 assert(r.begin(640,480));waitFor(r,State::Recording);assert(!r.begin(640,480));
 // Camera buffers can contain joined JPEGs or stale trailing data. Reject them
 // without publishing a slot; a subsequent valid frame must still save normally.
 auto joined=jpeg;joined.insert(joined.end(),jpeg.begin(),jpeg.end());
 assert(!r.submit(joined.data(),joined.size(),1));
 auto trailing=jpeg;trailing.push_back(0);
 assert(!r.submit(trailing.data(),trailing.size(),2));
 assert(!r.submit(jpeg.data(),jpeg.size()-1,3));
 assert(r.submissionStats().invalid==3&&r.submissionStats().accepted==0);
 // Drain each frame to make normal-path counts deterministic, including odd padding.
 for(unsigned i=0;i<4;++i){assert(r.submit(jpeg.data(),jpeg.size(),100+i*200));while(r.frames()!=i+1)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
 r.stop();r.close();join();assert(r.state()==State::Saved&&r.frames()==4&&!allocations);
 const auto& b=files.at(r.path());assert(u32(b,4)==b.size()-8&&u32(b,48)==4);
 assert(u32(b,128)==800&&u32(b,132)==4000);
 const size_t idx=224+4*core::avi::chunkBytes(jpeg.size());assert(!memcmp(b.data()+idx,"idx1",4));
 for(unsigned i=0;i<4;++i){size_t entry=idx+8+i*16;assert(u32(b,entry+8)==4+i*core::avi::chunkBytes(jpeg.size()));assert(u32(b,entry+12)==jpeg.size());}
 std::ofstream(argv[1],std::ios::binary).write(reinterpret_cast<const char*>(b.data()),b.size());
 // No-frame stop is a failure, not an empty successful video; close is idempotent.
 assert(r.begin(320,240));r.stop();r.close();join();assert(r.state()==State::Failed);r.close();assert(!allocations);
 // Worker stalled on SD: producer queue stays bounded; stop drains accepted data.
 assert(r.begin(640,480));waitFor(r,State::Recording);blockWrites=true;
 assert(r.dropped()==0&&r.submissionStats().accepted==0&&r.submissionStats().maxQueued==0);
 std::vector<std::vector<uint8_t>> queued;
 for(unsigned i=0;i<6;++i){
  auto borrowed=marked(uint8_t(i));queued.push_back(borrowed);
  assert(r.submit(borrowed.data(),borrowed.size(),i*33));
  std::fill(borrowed.begin(),borrowed.end(),0); // Producer may immediately reuse its input.
 }
 assert(!r.submit(jpeg.data(),jpeg.size(),600));assert(r.dropped()==1);
 assert(!r.submit(nullptr,4,600));assert(!r.submit(jpeg.data(),3,600));
 const uint8_t invalid[]={0,0,0,0};assert(!r.submit(invalid,sizeof(invalid),600));
 // The size check must precede access to the deliberately tiny borrowed array.
 assert(!r.submit(invalid,core::avi::kJpegFrameBytes+1,600));
 const auto stats=r.submissionStats();
 assert(stats.accepted==6&&stats.queueFull==1&&stats.invalid==3&&stats.oversized==1&&stats.maxQueued==6);
 assert(r.dropped()==5);
 r.stop();assert(!r.submit(jpeg.data(),jpeg.size(),800));blockWrites=false;r.close();join();assert(r.state()==State::Saved&&r.frames()==6&&!allocations);
 assert(r.dropped()==5&&r.submissionStats().accepted==6);
 const auto& drained=files.at(r.path());size_t at=core::avi::kHeaderBytes;
 for(const auto& expected:queued){
  assert(!memcmp(drained.data()+at,"00dc",4)&&u32(drained,at+4)==expected.size());
  assert(!memcmp(drained.data()+at+8,expected.data(),expected.size()));
  at+=core::avi::chunkBytes(expected.size());
 }
 assert(!memcmp(drained.data()+at,"idx1",4)&&u32(drained,at+4)==6*16);
 // Actual write/seek/rename/close failures are never reported as Saved.
 for(int mode=0;mode<4;++mode){
   writes=0;failWrite=mode==0?3:0;failSeek=mode==1;failRename=mode==2;failClose=mode==3;
   assert(r.begin(640,480));waitFor(r,State::Recording);
   assert(r.submit(jpeg.data(),jpeg.size(),0));r.stop();r.close();join();
   assert(r.state()==State::Failed&&!allocations);assert(!files.count(r.path()));
  if(failClose)assert(files.count(std::string(r.path())+".part"));
 }
 failWrite=0;failSeek=failRename=failClose=false;
 // Timestamp arithmetic crosses millis wrap; duration limit finalizes automatically.
 assert(r.begin(640,480));waitFor(r,State::Recording);
 assert(r.submit(jpeg.data(),jpeg.size(),0xfffffff0));
 while(r.frames()!=1)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 assert(r.submit(jpeg.data(),jpeg.size(),uint32_t(0xfffffff0u+Recording::kMaxDurationMs)));
 waitFor(r,State::Saved);r.close();join();assert(!allocations);
 const auto& limited=files.at(r.path());
 assert(u32(limited,128)==Recording::kMaxDurationMs && u32(limited,132)==2000);
 core::avi::Info limitedInfo;
 assert(core::avi::parse(limited.data(),limited.size(),limitedInfo));
 assert(uint64_t(limitedInfo.frames)*limitedInfo.intervalUs==uint64_t(Recording::kMaxDurationMs)*1000);
 // Combined recording uses a bounded stream, not a full-duration PCM allocation.
 services::camera::Session camera;
 camera.active=false;assert(!r.beginWithAudio(camera,640,480));camera.active=true;
 for(int mode=0;mode<4;++mode){
  failMicAdmission=mode==0;failMicInit=mode==1;failMicSpawn=mode==2;failSpawn=mode==3;
  assert(!r.beginWithAudio(camera,640,480));join();assert(!allocations&&!micOwners&&!micLeases);
 }
 failMicAdmission=failMicInit=failMicSpawn=failSpawn=false;
 assert(r.beginWithAudio(camera,640,480));waitFor(r,State::Recording);
 for(unsigned i=0;i<4;++i){
  assert(r.submit(jpeg.data(),jpeg.size(),millis()));
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
 }
 r.stop();r.close();join();assert(r.state()==State::Saved&&!allocations&&!micOwners&&!micLeases);
 const auto combined=files.at(r.path());core::avi::Info av;
 assert(core::avi::parse(combined.data(),combined.size(),av,324)&&av.audioSamples==micSamples);
 assert(av.frames>=av.audioSamples/1600&&av.frames<=av.audioSamples/1600+2);
 assert(u32(combined,128)==1&&u32(combined,132)==30);
 std::ofstream(std::string(argv[1])+".audio.avi",std::ios::binary).write(reinterpret_cast<const char*>(combined.data()),combined.size());
 // A second image in an already occupied timeline slot is not published.
 // Subsequent holds must repeat the last published image, not this discarded
 // submission. Distinct payload markers make the old substitution observable.
 assert(r.beginWithAudio(camera,640,480));waitFor(r,State::Recording);
 const uint32_t sameTimestamp=millis();
 const auto firstImage=marked(0x11);
 const auto discardedImage=marked(0x22);
 const auto nextImage=marked(0x33);
 assert(r.submit(firstImage.data(),firstImage.size(),sameTimestamp));
 assert(r.submit(discardedImage.data(),discardedImage.size(),sameTimestamp));
 assert(r.submit(nextImage.data(),nextImage.size(),sameTimestamp+100));
 std::this_thread::sleep_for(std::chrono::milliseconds(120));
 r.stop();r.close();join();assert(r.state()==State::Saved&&!allocations&&!micOwners&&!micLeases);
 const auto& coalesced=files.at(r.path());core::avi::Info coalescedInfo;
 assert(core::avi::parse(coalesced.data(),coalesced.size(),coalescedInfo,324));
 bool sawFirst=false,sawNext=false;
 for(uint32_t at=coalescedInfo.headerBytes;at<coalescedInfo.movieEnd;) {
  core::avi::Chunk chunk;assert(core::avi::parseChunk(coalesced.data()+at,at,coalescedInfo,chunk));
  if(!chunk.audio && chunk.bytes) {
   const uint8_t marker=coalesced[at+14];
   assert(marker!=0x22); // A discarded image must never emerge as a hold.
   if(marker==0x11){assert(!sawNext);sawFirst=true;}
   else {assert(marker==0x33&&sawFirst);sawNext=true;}
  }
  at=chunk.nextOffset;
 }
 assert(sawFirst&&sawNext);
 // Cross the PCM ring repeatedly with 80 ms JPEG writes and sparse updates.
 // Microphone input must keep draining even when video throughput is low.
 micLimit=160000;videoWriteDelayMs=80;
 assert(r.beginWithAudio(camera,640,480));waitFor(r,State::Recording);
 for(unsigned i=0;i<50 && micSamples<150000;++i){
  const auto& image=i%2?alternate:jpeg;
  r.submit(image.data(),image.size(),millis()); // JPEG overload may drop; PCM must remain exact.
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
 }
 r.stop();r.close();join();assert(r.state()==State::Saved&&r.audioSamples()>96000);
 assert(r.audioSamples()==micSamples&&!allocations&&!micOwners&&!micLeases);
 const auto& wrapped=files.at(r.path());
 // Holds must carry no JPEG payload, while preserving every timeline interval.
 core::avi::Info sparse;assert(core::avi::parse(wrapped.data(),wrapped.size(),sparse,324));
 unsigned holds=0,images=0,jpegPayload=0,pcmPayload=0;
 for(uint32_t at=sparse.headerBytes;at<sparse.movieEnd;) {
  core::avi::Chunk chunk;assert(core::avi::parseChunk(wrapped.data()+at,at,sparse,chunk));
  if(!chunk.audio){if(chunk.bytes){++images;jpegPayload+=chunk.bytes;}else{assert(images);++holds;}}
  else pcmPayload+=chunk.bytes;
  at=chunk.nextOffset;
 }
 assert(holds>images && holds+images==sparse.frames);
 const auto writer=r.writerStats();
 assert(writer.jpegChunks==images && writer.jpegBytes==jpegPayload);
 assert(writer.pcmBytes==pcmPayload && pcmPayload==r.audioSamples()*2);
 assert(writer.repeatedJpegChunks<images);
 assert(writer.repeatedJpegChunks<=3); // Initial fill plus at most two final samples.
 assert(writer.repeatedJpegBytes<=3*std::max(jpeg.size(),alternate.size()));
 assert(writer.maxChunkWriteMs>=80 && writer.chunkWriteMs>=images*80);


 std::ofstream(std::string(argv[1])+".wrapped.avi",std::ios::binary).write(reinterpret_cast<const char*>(wrapped.data()),wrapped.size());
 micLimit=9600;videoWriteDelayMs=0;
 // Immediate cancellation and re-entry release RX, transport and all buffers.
 assert(r.beginWithAudio(camera,640,480));assert(r.writerStats().jpegChunks==0);r.stop();r.close();join();
 assert(r.state()==State::Failed&&!allocations&&!micOwners&&!micLeases);
 r.close();assert(!micOwners&&!micLeases);
 // Combined writes, final header seek and rename use the same failure contract.
 for(int mode=0;mode<4;++mode){
  writes=0;failWrite=mode==0?3:0;failSeek=mode==1;failRename=mode==2;failClose=mode==3;
  assert(r.beginWithAudio(camera,640,480));waitFor(r,State::Recording);
  assert(r.submit(jpeg.data(),jpeg.size(),millis()));r.stop();r.close();join();
  assert(r.state()==State::Failed&&!allocations&&!micOwners&&!micLeases);
  assert(!files.count(r.path()));
  if(failClose)assert(files.count(std::string(r.path())+".part"));
 }
 failWrite=0;failSeek=failRename=failClose=false;
 // A slow SD consumer cannot silently lose PCM and advertise a successful clip.
 blockWrites=true;fastMic=true;micLimit=96000;
 assert(r.beginWithAudio(camera,640,480));
 while(micSamples<=48000)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 blockWrites=false;waitFor(r,State::Failed);r.close();join();
 assert(strstr(r.error(),"overrun")&&!allocations&&!micOwners&&!micLeases);
 fastMic=false;micLimit=9600;failMicRead=true;
 assert(r.beginWithAudio(camera,640,480));waitFor(r,State::Failed);r.close();join();
 assert(strstr(r.error(),"read failed")&&!allocations&&!micOwners&&!micLeases);failMicRead=false;
 std::cout<<"PASS: recording lifecycle, queue overload, AVI index/padding, allocation/spawn/I/O failures, duration limit and wraparound\n";
}
