"""Compile real mixed catalog and bounded AVI thumbnail reader with faultable SD."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as td:
 p=Path(td)
 def write(name,text):
  f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
 write('hal/hal_bus.h','namespace hal {namespace bus {inline void lockSpi(){} inline void unlockSpi(){}}}')
 write('hal/hal_sd.h','''#pragma once
#include <cstdint>
#include <cstddef>
namespace hal {namespace sd {
struct StreamReader {void* handle=nullptr;size_t size=0,position=0;};
bool mounted();bool removeFile(const char*);
bool openStream(const char*,StreamReader*);size_t readStream(StreamReader*,uint8_t*,size_t);bool seekStream(StreamReader*,size_t);void closeStream(StreamReader*);
}}
''')
 write('SD.h',r'''#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <ctime>
#define FILE_READ 0
inline unsigned micros(){static unsigned n=0;return ++n;}
struct String:std::string {using std::string::string;String(std::string s):std::string(s){}};
extern std::map<std::string,std::vector<std::string>> dirs;
extern std::map<std::string,time_t> timestamps;
struct File {
 std::string path;size_t at=0;bool valid=false;
 operator bool()const{return valid;}bool isDirectory()const{return dirs.count(path);}
 String getNextFileName(bool* directory){*directory=false;auto& names=dirs[path];return at<names.size()?String(names[at++]):String();}
 time_t getLastWrite(){return timestamps[path];}
};
struct SDClass {bool exists(const char* p){return dirs.count(p)||timestamps.count(p);}
 File open(const char* p,int=0){return {p,0,exists(p)};}};extern SDClass SD;
''')
 write('main.cpp',r'''
#include "SD.h"
#include "hal/hal_photo_catalog.h"
#include "services/video_thumbnail.h"
#include "hal/hal_sd.h"
#include "core/mjpeg_avi.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <algorithm>
std::map<std::string,std::vector<std::string>> dirs;
std::map<std::string,time_t> timestamps;SDClass SD;
bool allocation=true,removeOk=true,mounted=true;int streams=0,allocations=0;
std::vector<uint8_t> bytes;size_t cut=SIZE_MAX;
namespace core {namespace memory {
void* allocPsram(const char*,size_t n){if(!allocation)return nullptr;++allocations;return malloc(n);}
void freePsram(void*& p){if(p){--allocations;free(p);p=nullptr;}}
}}
namespace hal {namespace sd {
bool mounted(){return ::mounted;}bool removeFile(const char*p){return removeOk && timestamps.erase(p);}
bool openStream(const char*,StreamReader*r){r->size=bytes.size();r->position=0;++streams;return true;}
size_t readStream(StreamReader*r,uint8_t*out,size_t n){n=std::min({n,size_t(7),r->size-r->position,cut>r->position?cut-r->position:0});memcpy(out,bytes.data()+r->position,n);r->position+=n;return n;}
bool seekStream(StreamReader*r,size_t at){if(at>r->size)return false;r->position=at;return true;}
void closeStream(StreamReader*){--streams;}
}}
void movie(bool audio){using namespace core::avi;
 const uint32_t h=audio?kAudioHeaderBytes:kHeaderBytes,pcm=audio?12:0,m=pcm+14,entries=audio?2:1;
 bytes.assign(h+m+8+entries*16,0);
 if(audio)audioHeader(bytes.data(),1280,720,1,1000,m,6,48000,entries);
 else header(bytes.data(),640,480,1,1000,m,6);
 uint32_t pos=h;if(audio){chunk(bytes.data()+pos,4,true);pos+=12;}
 chunk(bytes.data()+pos,6);uint8_t jpeg[]={255,216,1,2,255,217};memcpy(bytes.data()+pos+8,jpeg,6);
}
int main(){using namespace hal::sd;
 dirs["/photos"]={"IMG_00009.jpg","IMG_00010.jpg",".hidden.jpg","no.txt"};
 dirs["/videos"]={"VID_a.AVI","unfinished.avi.part"};
 timestamps={{"/photos/IMG_00009.jpg",10},{"/photos/IMG_00010.jpg",30},{"/videos/VID_a.AVI",20}};
 {PhotoCatalog c;assert(c.load(true)==PhotoCatalogResult::Ok&&c.count()==3);
 assert(!strcmp(c[0].path,"/photos/IMG_00010.jpg"));assert(c[1].video&&!strcmp(c[1].path,"/videos/VID_a.AVI"));assert(!c[2].video);
 removeOk=false;assert(!c.removeAt(1)&&c.count()==3);assert(c.removeAt(1,true)&&c.count()==2);
 c.clear();allocation=false;assert(c.load(true)==PhotoCatalogResult::NoMemory);allocation=true;
 dirs.erase("/photos");assert(c.load(true)==PhotoCatalogResult::Ok&&c.count()==1&&c[0].video);
 assert(c.load()==PhotoCatalogResult::Ok&&c.count()==0);
 ::mounted=false;assert(c.load(true)==PhotoCatalogResult::NoSd);::mounted=true;}
 for(bool audio:{false,true}){movie(audio);services::video::Thumbnail t;
 assert(t.load("/videos/x.avi")&&t.size()==6&&t.durationMs()==1000&&streams==0);
 assert(t.data()[0]==255&&t.data()[1]==216);t.clear();assert(!t.data());
 cut=100;assert(!t.load("x")&&!t.data()&&streams==0);cut=SIZE_MAX;
 allocation=false;assert(!t.load("x")&&!t.data()&&streams==0);allocation=true;
 bytes[0]=0;assert(!t.load("x")&&streams==0);
 }
 assert(allocations==0&&streams==0);
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(ROOT/'src/hal/hal_photo_catalog.cpp'),str(ROOT/'src/services/video_thumbnail.cpp'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS mixed gallery ordering/filtering/removal, missing directories, thumbnail PCM skip, partial reads, corruption, allocation and cleanup')
