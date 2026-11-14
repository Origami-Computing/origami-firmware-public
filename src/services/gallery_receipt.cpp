#include "services/gallery_receipt.h"
#include "services/video_upload_state.h"
#include "hal/hal_sd.h"
#include <cstdio>
#include <cstring>
namespace services { namespace gallery_receipt {
namespace {
bool describe(const char* path,const char* account,uint32_t bytes,uint32_t modified,
              char (&marker)[128],char (&contents)[112]) {
    if(!path || !account || !account[0] || !bytes ||
       (strncmp(path,"/photos/",8) && strncmp(path,"/videos/",8))) return false;
    const int n=snprintf(marker,sizeof(marker),"/upload_queue/%c-%s.receipt",path[1],path+8);
    if(n<0 || size_t(n)>=sizeof(marker)) return false;
    snprintf(contents,sizeof(contents),"v1 %s %lu %lu\n",account,(unsigned long)bytes,(unsigned long)modified);
    return true;
}
}
bool confirmed(const char* path) {
    char account[65],marker[128],expected[112],actual[113];
    size_t bytes=0; uint32_t modified=0;
    if(!video_upload::binding(core::secrets::backendConfig(),account) ||
       !hal::sd::fileInfo(path,bytes,modified) ||
       !describe(path,account,bytes,modified,marker,expected)) return false;
    const size_t n=hal::sd::readFile(marker,actual,sizeof(actual));
    return n==strlen(expected) && !memcmp(actual,expected,n);
}
void remember(const char* path,const char* account,uint32_t bytes,uint32_t modified) {
    char marker[128],contents[112];
    if(describe(path,account,bytes,modified,marker,contents))
        hal::sd::writeFile(marker,reinterpret_cast<const uint8_t*>(contents),strlen(contents));
}
} }
