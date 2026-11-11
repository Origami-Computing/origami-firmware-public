#include "hal/app_storage.h"
#include "hal/hal_sd.h"
#include "hal/hal_bus.h"
#include <SD.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <algorithm>

namespace hal { namespace app_storage {
namespace {
bool flashMounted = false;
// No lock remains held between filesystem operations/chunks.
struct Bus {
    Medium medium; bool ok;
    explicit Bus(Medium m):medium(m),ok(m != Medium::Sd || hal::bus::lockSpi()) {}
    ~Bus(){if(medium==Medium::Sd && ok)hal::bus::unlockSpi();}
};
bool pathFor(Medium m,const char* path,char (&out)[240]) {
    if(!path || path[0]=='/' || strlen(path)>180 || strstr(path,"..") || strchr(path,'\\'))return false;
    for(const char* p=path;*p;++p)if(uint8_t(*p)<32 || uint8_t(*p)>126)return false;
    return snprintf(out,sizeof(out),"%s%s%s",m==Medium::Sd?"/sd/custom_apps":"/customapps",*path?"/":"",path)<int(sizeof(out));
}
const esp_partition_t* partition() {
    const auto* p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"customapps");
    uint32_t size=0;
    if(!p || p->address!=kFlashOffset || p->size!=kFlashBytes ||
       p->subtype!=ESP_PARTITION_SUBTYPE_DATA_SPIFFS || esp_flash_get_size(nullptr,&size)!=ESP_OK ||
       size<kFlashOffset+kFlashBytes)return nullptr;
    return p;
}
Result errorResult(){return errno==ENOENT?Result::Missing:Result::Io;}
}
bool hasFlashStore() { return partition()!=nullptr; }
Result mount(Medium m) {
    if(m==Medium::Sd) {
        if(!hal::sd::mounted())return Result::Unavailable;
        return hal::sd::ensureDir("/custom_apps")?Result::Ok:Result::Io;
    }
    if(flashMounted)return Result::Ok;
    if(!partition())return Result::Unavailable;
    flashMounted=LittleFS.begin(false,"/customapps",8,"customapps");
    return flashMounted?Result::Ok:Result::Corrupt;
}
Result initializeBlankFlash(Cancel cancel) {
    if(mount(Medium::Flash)==Result::Ok)return Result::Ok;
    const auto* p=partition();if(!p)return Result::Unavailable;
    uint8_t block[256];
    for(size_t at=0;at<p->size;at+=sizeof(block)) {
        if(cancel && cancel())return Result::Cancelled;
        if(esp_partition_read(p,at,block,sizeof(block))!=ESP_OK)return Result::Io;
        for(uint8_t b:block)if(b!=0xff)return Result::Corrupt;
        if((at%4096)==0)vTaskDelay(1);
    }
    // begin(false) selected the verified label even after mount failure.
    if(!LittleFS.format())return Result::Io;
    return mount(Medium::Flash);
}
Result read(Medium m,const char* path,uint8_t* out,size_t cap,size_t& size) {
    size=0;char full[240];if(!pathFor(m,path,full)||!out)return Result::Io;
    int fd;
    {Bus guard(m);if(!guard.ok)return Result::Io;fd=::open(full,O_RDONLY);if(fd<0)return errorResult();}
    Result result=Result::Ok;
    struct stat st{};
    {Bus guard(m);if(!guard.ok || fstat(fd,&st)!=0 || !S_ISREG(st.st_mode))result=Result::Io;
     else if(st.st_size<0 || uint64_t(st.st_size)>cap)result=Result::TooLarge;}
    if(result==Result::Ok) {
        const size_t expected=size_t(st.st_size);
        while(size<expected){Bus guard(m);const ssize_t n=guard.ok?::read(fd,out+size,std::min(size_t(4096),expected-size)):-1;
            if(n<=0){result=Result::Io;break;}size+=size_t(n);}
    }
    {Bus guard(m);if(!guard.ok || ::close(fd)!=0)result=Result::Io;}
    if(result!=Result::Ok)size=0;
    return result;
}
Result write(Medium m,const char* path,const uint8_t* bytes,size_t count,Cancel cancel) {
    char full[240];if(!pathFor(m,path,full)||(!bytes&&count))return Result::Io;
    int fd;
    {Bus guard(m);if(!guard.ok)return Result::Io;fd=::open(full,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return Result::Io;}
    Result result=Result::Ok;
    for(size_t at=0;at<count;) {
        if(cancel && cancel()){result=Result::Cancelled;break;}
        const size_t n=std::min(size_t(4096),count-at);
        {Bus guard(m);if(!guard.ok || ::write(fd,bytes+at,n)!=ssize_t(n)){result=Result::Io;break;}}
        at+=n;vTaskDelay(1);
    }
    {Bus guard(m);if(!guard.ok || (result==Result::Ok && fsync(fd)!=0))result=Result::Io;
     if(::close(fd)!=0)result=Result::Io;}
    return result;
}
Result mkdir(Medium m,const char* path){char full[240];if(!pathFor(m,path,full))return Result::Io;
    Bus guard(m);if(!guard.ok)return Result::Io;if(::mkdir(full,0700)==0)return Result::Ok;
    struct stat st{};return errno==EEXIST && stat(full,&st)==0 && S_ISDIR(st.st_mode)?Result::Ok:Result::Io;}
Result remove(Medium m,const char* path){char full[240];if(!pathFor(m,path,full))return Result::Io;
    Bus guard(m);if(!guard.ok)return Result::Io;return ::unlink(full)==0?Result::Ok:errorResult();}
Result freeBytes(Medium m,uint64_t& out){Bus guard(m);if(!guard.ok)return Result::Io;
    uint64_t total=m==Medium::Sd?SD.totalBytes():LittleFS.totalBytes();
    uint64_t used=m==Medium::Sd?SD.usedBytes():LittleFS.usedBytes();
    if(!total || used>total)return Result::Io;out=total-used;return Result::Ok;}
Result listDirs(Medium m,const char* path,char* names,size_t count,size_t stride,bool& overflow){
    overflow=false;if(!names||stride<2)return Result::Io;memset(names,0,count*stride);
    char full[240];if(!pathFor(m,path,full))return Result::Io;
    Bus guard(m);if(!guard.ok)return Result::Io;
    DIR* dir=opendir(full);if(!dir)return errorResult();size_t found=0;Result result=Result::Ok;
    errno=0;
    while(auto* item=readdir(dir)){
        if(item->d_name[0]=='.')continue;
        char child[280];struct stat st{};
        if(snprintf(child,sizeof(child),"%s/%s",full,item->d_name)>=int(sizeof(child)) || stat(child,&st)!=0){result=Result::Io;break;}
        if(!S_ISDIR(st.st_mode))continue;
        if(strlen(item->d_name)>=stride){overflow=true;continue;}
        if(found==count){overflow=true;break;}
        strcpy(names+found++*stride,item->d_name);errno=0;
    }
    if(errno)result=Result::Io;
    if(closedir(dir)!=0)result=Result::Io;
    return result;
}
bool protectedSdPath(const char* path){
    // FAT short-name aliases (for example CUSTOM~1) can name the managed root
    // without spelling custom_apps. Generic mutation must not accept aliases.
    if(!path || path[0]!='/' || strstr(path,"..") || strchr(path,'~') || strchr(path,'\\') || strstr(path,"//"))return true;
    if(!strcmp(path,"/"))return true;
    for(const char* p=path;*p;++p){
        if(*p=='/' && p[1]=='.')return true;
        if((*p=='.'||*p==' ') && (p[1]=='/'||p[1]==0))return true;
    }
    constexpr const char* root="/custom_apps";const size_t n=strlen(root);
    return !strncasecmp(path,root,n) && (path[n]=='/'||path[n]==0);
}
} }
