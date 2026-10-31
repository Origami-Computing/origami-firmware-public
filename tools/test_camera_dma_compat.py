"""Exercise the actual pinned-camera DMA adapter with controlled SDK calls.

This checks geometry, allocation thresholds and mode isolation, not hardware
throughput or the private driver's ABI (which remains a pinned-build contract).
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='camera-dma-compat-') as directory:
    root = Path(directory)
    headers = {
        'esp_arduino_version.h': '#define ESP_ARDUINO_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n#define ESP_ARDUINO_VERSION 20017\n',
        'esp_camera.h': '#pragma once\nusing esp_err_t=int;using framesize_t=int;constexpr int PIXFORMAT_JPEG=4,PIXFORMAT_RGB565=0,FRAMESIZE_QVGA=5;struct camera_config_t {int pixel_format;int xclk_freq_hz;};\n',
        'esp_heap_caps.h': '#include <stddef.h>\n#define MALLOC_CAP_DMA 8\nsize_t heap_caps_get_largest_free_block(unsigned);\n',
        'esp_log.h': '#define ESP_LOGW(...) ((void)0)\n',
    }
    for name, content in headers.items():
        (root/name).write_text(content)
    (root/'main.cpp').write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "esp_camera.h"
#include "esp_heap_caps.h"
extern "C" bool __wrap_ll_cam_dma_sizes(void*);
extern "C" int __wrap_cam_config(const camera_config_t*,int,uint16_t);
struct Camera {uint32_t item=1,buffer=16384,chunk=1024,count=16,node=1024;uint32_t untouched=0xabcdef12;};
Camera camera;
size_t largest=50000;bool sdkOk=true;int sdkResult=0,calls=0,heapQueries=0;
size_t heap_caps_get_largest_free_block(unsigned caps){assert(caps==MALLOC_CAP_DMA);++heapQueries;return largest;}
extern "C" bool __real_ll_cam_dma_sizes(void*){++calls;return sdkOk;}
extern "C" int __real_cam_config(const camera_config_t*,int size,uint16_t pid){
 assert(size==5&&pid==0x3660);assert(__wrap_ll_cam_dma_sizes(&camera)==sdkOk);return sdkResult;
}
int main(){
 const camera_config_t jpeg{4,20000000};
 for(size_t available:{size_t(0),size_t(4096),size_t(5119),size_t(5120),size_t(7412),size_t(8180),size_t(9215),size_t(9216),size_t(17407),size_t(17408),size_t(50000)}){
  camera={};largest=available;heapQueries=0;
  assert(__wrap_cam_config(&jpeg,5,0x3660)==0&&heapQueries==1);
  const unsigned expected=available>=17408?16384:available>=5120&&available<9216?4096:8192;
  assert(camera.buffer==expected&&camera.count==expected/1024);
  assert(camera.item==1&&camera.chunk==1024&&camera.node==1024&&camera.untouched==0xabcdef12);
  // A later call outside cam_config cannot inherit the JPEG-only override.
  camera={};heapQueries=0;assert(__wrap_ll_cam_dma_sizes(&camera));assert(camera.buffer==16384&&heapQueries==0);
 }
 for(const camera_config_t* config:{&jpeg,static_cast<const camera_config_t*>(nullptr)}){
  for(auto member:{&Camera::item,&Camera::buffer,&Camera::chunk,&Camera::count,&Camera::node}){
   camera={};camera.*member+=1;Camera before=camera;heapQueries=0;largest=7412;
   assert(__wrap_cam_config(config,5,0x3660)==0);assert(!memcmp(&camera,&before,sizeof camera)&&heapQueries==0);
  }
 }
 for(camera_config_t config: {camera_config_t{4,16000000},camera_config_t{0,20000000}}){
  camera={};heapQueries=0;assert(__wrap_cam_config(&config,5,0x3660)==0);assert(camera.buffer==16384&&heapQueries==0);
 }
#ifdef ORIGAMI_RGB_PREVIEW_TRIAL
 const camera_config_t rgb{0,24000000};
 for(size_t available:{size_t(13823),size_t(13824),size_t(15860),size_t(31743),size_t(31744)}){
  camera={1,30720,15360,2,3840,0xabcdef12};largest=available;heapQueries=0;
  assert(__wrap_cam_config(&rgb,5,0x3660)==0&&heapQueries==1);
  const bool changed=available>=13824&&available<31744;
  assert(camera.buffer==(changed?12800u:30720u)&&camera.chunk==(changed?6400u:15360u));
  assert(camera.node==(changed?1280u:3840u)&&camera.count==2&&camera.item==1&&camera.untouched==0xabcdef12);
  assert(camera.buffer==camera.chunk*camera.count&&camera.chunk%camera.node==0&&153600%camera.chunk==0);
  camera={1,30720,15360,2,3840,0xabcdef12};heapQueries=0;
  assert(__wrap_ll_cam_dma_sizes(&camera)&&camera.buffer==30720&&heapQueries==0);
 }
 for(auto member:{&Camera::item,&Camera::buffer,&Camera::chunk,&Camera::count,&Camera::node}){
  camera={1,30720,15360,2,3840,0xabcdef12};camera.*member+=1;Camera before=camera;largest=15860;heapQueries=0;
  assert(__wrap_cam_config(&rgb,5,0x3660)==0&&!memcmp(&camera,&before,sizeof camera)&&heapQueries==0);
 }
 for(camera_config_t config:{camera_config_t{0,16000000},camera_config_t{0,20000000},camera_config_t{4,24000000}}){
  camera={1,30720,15360,2,3840,0xabcdef12};Camera before=camera;heapQueries=0;
  assert(__wrap_cam_config(&config,5,0x3660)==0&&!memcmp(&camera,&before,sizeof camera)&&heapQueries==0);
 }
#endif
 sdkOk=false;sdkResult=-1;camera={};heapQueries=0;
 assert(__wrap_cam_config(&jpeg,5,0x3660)==-1&&camera.buffer==16384&&heapQueries==0);
 sdkOk=true;assert(__wrap_ll_cam_dma_sizes(nullptr));
 camera={};assert(__wrap_ll_cam_dma_sizes(&camera)&&camera.buffer==16384);
}
''')
    for flags in ([],['-DORIGAMI_RGB_PREVIEW_TRIAL']):
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-fsanitize=address,undefined',*flags,
                        '-I'+str(root),str(ROOT/'src/hal/camera_dma_compat.cpp'),
                        str(root/'main.cpp'),'-o',str(root/'test')],check=True)
        subprocess.run([str(root/'test')],check=True)
print('PASS actual camera DMA adapter: fragmented-heap thresholds, geometry, SDK failures and JPEG/mode isolation under sanitizers')
