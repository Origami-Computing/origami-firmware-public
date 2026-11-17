"""Exercise production EXIF/storage/HAL writes and independently parse saved JPEGs."""
from pathlib import Path
import io
import subprocess
import tempfile
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


with tempfile.TemporaryDirectory(prefix="cine-exif-") as td:
    p = Path(td)
    (p / "services").mkdir()
    (p / "Arduino.h").write_text("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n")
    (p / "SPI.h").write_text("#pragma once\n")
    (p / "services/uploads.h").write_text(r'''#pragma once
#include <stddef.h>
#include <stdint.h>
namespace services {namespace uploads {
struct Queue {uint32_t generatePath(const char*,const char*,char*,size_t){return 0;}
 bool enqueue(const char*,const char*,const char*,bool,uint32_t){return false;}};
inline bool ensureInit(){return true;} inline Queue& queue(){static Queue q;return q;}
}}
''')
    source = (ROOT / "src/hal/hal_sd.cpp").read_text()
    cpp = r'''
#include "core/cine_exif.h"
#include "core/cine_isp.h"
#include "services/photo_storage.h"
#include "hal/hal_sd.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
namespace core {namespace time {bool isSynced(){return false;}}}
std::map<std::string,std::vector<uint8_t>> files;
int locks=0,opens=0,closes=0,writes=0,failPart=-1;bool failOpen=false;
struct File {
 std::string path;bool valid;
 explicit operator bool()const{return valid;}
 size_t write(const uint8_t* data,size_t n){assert(locks==1);const bool fail=writes++==failPart;const size_t count=fail?n-1:n;files[path].insert(files[path].end(),data,data+count);return count;}
 void close(){assert(locks==1);++closes;}
};
struct FakeSD {File open(const char* path,int){assert(locks==1);++opens;if(!failOpen)files[path].clear();return {path,!failOpen};}}SD;
constexpr int FILE_WRITE=0;
namespace hal {namespace bus {void lockSpi(){assert(locks++==0);}void unlockSpi(){assert(--locks==0);}}
namespace sd {static bool s_mounted=true;bool ensureMediaMounted(){return s_mounted;}bool exists(const char* p){return files.count(p);}
 bool mounted(){return s_mounted;}bool ensureDir(const char*){return true;}
 bool removeFile(const char* p){assert(locks==0);return files.erase(p)>0;}
'''
    cpp += function(source, "bool writeFileParts(") + "\n" + function(source, "bool writeFile(")
    cpp += r'''
}}
uint16_t u16(const uint8_t* p){return p[0]|uint16_t(p[1])<<8;}
uint32_t u32(const uint8_t* p){return u16(p)|uint32_t(u16(p+2))<<16;}
void checkOffsets(const uint8_t* block,size_t size){
 const uint8_t* t=block+10;
 for(uint32_t at:{u32(t+4),u32(t+8+2+4*12+8)}){
  assert(at%2==0&&at+2<size-10);unsigned n=u16(t+at);uint16_t previous=0;
  for(unsigned i=0;i<n;++i){const auto* entry=t+at+2+i*12;auto tag=u16(entry);assert(tag>previous);previous=tag;
   auto type=u16(entry+2);auto count=u32(entry+4);size_t bytes=count*(type==3?2:type==4?4:type==5?8:1);
   if(bytes>4){auto offset=u32(entry+8);assert(offset%2==0&&offset+bytes<=size-10);}
  }
 }
}
int main(int argc,char** argv){assert(argc==3);std::ifstream input(argv[1],std::ios::binary);std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(input)),{});const auto original=jpeg;
 services::photos::Target target;target.sequence=1;strcpy(target.path,"/photo.jpg");
 core::cine::ExifInfo info{"v1.0.5","Sunny",40001,64,48,true};
 struct Guard {uint8_t before[16],data[core::cine::exifCapacity],after[16];}guard;
 memset(&guard,0xa5,sizeof(guard));auto size=core::cine::buildExif(guard.data,sizeof(guard.data),info);assert(size>0&&size<512);checkOffsets(guard.data,size);
 for(size_t capacity=0;capacity<size;++capacity){uint8_t small[512];memset(small,0xa5,sizeof(small));assert(!core::cine::buildExif(small,capacity,info));for(auto v:small)assert(v==0xa5);}
 for(auto v:guard.before)assert(v==0xa5);for(auto v:guard.after)assert(v==0xa5);
 auto bad=info;bad.firmwareVersion=nullptr;assert(!core::cine::buildExif(guard.data,512,bad));bad=info;bad.colorName=nullptr;assert(!core::cine::buildExif(guard.data,512,bad));bad=info;bad.width=0;assert(!core::cine::buildExif(guard.data,512,bad));
 std::string longVersion(90,'x');bad=info;bad.firmwareVersion=longVersion.c_str();assert(!core::cine::buildExif(guard.data,512,bad));assert(!core::cine::buildExif(nullptr,512,info));
 assert(!core::cine::exifInsertionOffset(nullptr,10));for(size_t n=0;n<4;++n)assert(!core::cine::exifInsertionOffset(jpeg.data(),n));
 auto malformed=jpeg;malformed[4]=malformed[5]=0xff;assert(!core::cine::exifInsertionOffset(malformed.data(),malformed.size()));
 malformed=jpeg;malformed[4]=malformed[5]=0;assert(!core::cine::exifInsertionOffset(malformed.data(),malformed.size()));
 const size_t insertion=core::cine::exifInsertionOffset(jpeg.data(),jpeg.size());assert(insertion>2);
 for(unsigned mode=0;mode<6;++mode){
  info.colorName=core::cine::colorProfile(core::cine::ColorMode(mode)).name;info.automatic=mode%2==0;info.exposureUs=mode==3?0:mode==4?UINT32_MAX:40001;
  size=core::cine::buildExif(guard.data,512,info);assert(size);checkOffsets(guard.data,size);
  opens=closes=writes=0;assert(services::photos::write(target,jpeg.data(),jpeg.size(),guard.data,size));assert(opens==1&&closes==1&&writes==3&&locks==0&&jpeg==original);
  const auto& saved=files[target.path];assert(saved.size()==jpeg.size()+size);
  assert(std::equal(jpeg.begin(),jpeg.begin()+insertion,saved.begin()));assert(std::equal(jpeg.begin()+insertion,jpeg.end(),saved.begin()+insertion+size));
  std::ofstream output(std::string(argv[2])+"/mode-"+std::to_string(mode)+".jpg",std::ios::binary);output.write((const char*)saved.data(),saved.size());
 }
 auto noApp0=jpeg;noApp0.erase(noApp0.begin()+2,noApp0.begin()+insertion);assert(core::cine::exifInsertionOffset(noApp0.data(),noApp0.size())==2);
 assert(services::photos::write(target,noApp0.data(),noApp0.size(),guard.data,size));
 for(int part=0;part<3;++part){writes=0;failPart=part;assert(!services::photos::write(target,jpeg.data(),jpeg.size(),guard.data,size));assert(!files.count(target.path)&&locks==0);}failPart=-1;
 failOpen=true;assert(!services::photos::write(target,jpeg.data(),jpeg.size(),guard.data,size)&&locks==0);failOpen=false;
 int before=opens;assert(!services::photos::write({},jpeg.data(),jpeg.size(),guard.data,size));assert(!services::photos::write(target,jpeg.data(),jpeg.size(),nullptr,size));assert(opens==before);
 uint8_t badBlock[512];memcpy(badBlock,guard.data,size);badBlock[2]=0;badBlock[3]=0;assert(!services::photos::write(target,jpeg.data(),jpeg.size(),badBlock,size));assert(opens==before);
 hal::sd::FilePart invalid[]={{jpeg.data(),1},{nullptr,1}};assert(!hal::sd::writeFileParts(target.path,invalid,2));assert(opens==before);
 hal::sd::FilePart overflow[]={{jpeg.data(),SIZE_MAX},{jpeg.data(),1}};assert(!hal::sd::writeFileParts(target.path,overflow,2));assert(opens==before);
 assert(!hal::sd::writeFileParts(target.path,nullptr,0));hal::sd::s_mounted=false;assert(!hal::sd::writeFile(target.path,jpeg.data(),jpeg.size()));assert(opens==before);hal::sd::s_mounted=true;
 writes=0;assert(services::photos::write(target,jpeg.data(),jpeg.size(),nullptr,0));assert(files[target.path]==jpeg&&writes==1);
}
'''
    (p / "test.cpp").write_text(cpp)
    photo = Image.new("RGB", (64, 48))
    photo.putdata([(x * 4, y * 5, (x + y) * 2) for y in range(48) for x in range(64)])
    photo.save(p / "original.jpg", quality=85)
    subprocess.run([
        "c++", "-std=c++11", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I" + str(p), "-I" + str(ROOT / "include"), str(p / "test.cpp"),
        str(ROOT / "src/core/cine_exif.cpp"), str(ROOT / "src/services/photo_storage.cpp"),
        "-o", str(p / "test")], check=True)
    subprocess.run([str(p / "test"), str(p / "original.jpg"), str(p)], check=True)
    original = Image.open(p / "original.jpg")
    original.load()
    for mode, name in enumerate(["Sunny", "Gold", "Portra", "Mono", "Cool", "Vivid"]):
        saved = Image.open(p / f"mode-{mode}.jpg")
        saved.load()
        assert saved.tobytes() == original.tobytes()
        exif = saved.getexif()
        assert exif[0x010F] == "Origami Computing" and exif[0x0110] == "Kami"
        assert exif[0x0131] == "kamiOS v1.0.5 / Cine Camera"
        assert exif[0x010E] == "Cine Camera / " + name
        fields = exif.get_ifd(0x8769)
        assert fields[0xA002] == 64 and fields[0xA003] == 48
        assert fields[0xA402] == mode % 2 and fields[0xA403] == 1
        assert fields[0x9000] == b"0232" and fields[0xA001] == 1
        if mode == 3:
            assert 0x829A not in fields
        else:
            exposure = fields[0x829A]
            assert exposure.numerator == (0xFFFFFFFF if mode == 4 else 40001)
            assert exposure.denominator == 1_000_000
        absent = {0x829D, 0x8827, 0x920A, 0xA405, 0xA431, 0xA432, 0xA433, 0xA434, 0xA435, 0x9003}
        assert not absent.intersection(fields) and 0x8825 not in exif
print("PASS production Cine EXIF + storage + SD multipart writes: independent tag/pixel parsing, bounds/alignment, unavailable exposure, all Color names, short writes, cleanup and original-byte preservation; ASan/UBSan")
