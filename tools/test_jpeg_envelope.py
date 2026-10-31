"""Exercise the production JPEG envelope guard with bounded malformed inputs."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=r'''
#include "core/jpeg_envelope.h"
#include <cassert>
#include <vector>
using core::images::singleJpegEnvelope;
int main(){
 std::vector<uint8_t> image={255,216,255,192,0,11,8,1,224,2,128,1,1,17,0,
  255,218,0,8,1,1,0,0,63,0,42,255,0,42,255,208,42,255,217};
 assert(singleJpegEnvelope(image.data(),image.size()));
 for(size_t n=0;n<image.size();++n)assert(!singleJpegEnvelope(image.data(),n));
 assert(!singleJpegEnvelope(nullptr,100));
 auto metadata=image;metadata.insert(metadata.begin()+2,{255,225,0,6,255,216,255,217});
 assert(singleJpegEnvelope(metadata.data(),metadata.size()));
 auto joined=image;joined.insert(joined.end(),image.begin(),image.end());
 assert(!singleJpegEnvelope(joined.data(),joined.size()));
 auto trailing=image;trailing.push_back(0);assert(!singleJpegEnvelope(trailing.data(),trailing.size()));
 auto bad=image;bad[4]=255;bad[5]=255;assert(!singleJpegEnvelope(bad.data(),bad.size()));
 bad=image;bad[4]=bad[5]=0;assert(!singleJpegEnvelope(bad.data(),bad.size()));
 const uint8_t empty[]={255,216,255,217};assert(!singleJpegEnvelope(empty,sizeof empty));
 bad=image;bad[3]=224;assert(!singleJpegEnvelope(bad.data(),bad.size()));
 bad=image;bad[16]=224;assert(!singleJpegEnvelope(bad.data(),bad.size()));
 bad=image;bad[27]=216;assert(!singleJpegEnvelope(bad.data(),bad.size()));
 // Random bounded input plus marker-dense cases must never over-read or loop.
 uint32_t state=1;
 for(size_t size=0;size<2048;++size){std::vector<uint8_t> bytes(size);
  for(auto& b:bytes){state=state*1664525+1013904223;b=uint8_t(state>>24);}
  singleJpegEnvelope(bytes.data(),bytes.size());
  for(auto& b:bytes)b=255;
  if(size>=2){bytes[0]=255;bytes[1]=216;}
  assert(!singleJpegEnvelope(bytes.data(),bytes.size()));
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(source)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production JPEG envelope guard: truncations, metadata markers, stuffing/restarts, joined/trailing payloads and bounded malformed input')
