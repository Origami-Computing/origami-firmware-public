#include "hal/hal_photo_catalog.h"

#include <SD.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ff.h"

#include "core/memory.h"
#include "hal/hal_bus.h"
#include "hal/hal_sd.h"

namespace hal {
namespace sd {

namespace {
struct SpiLock {
  SpiLock() { hal::bus::lockSpi(); }
  ~SpiLock() { hal::bus::unlockSpi(); }
};

const char* basename(const char* path) {
  if (!path) return "";
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

int comparePhotos(const void* left, const void* right) {
  const auto& a = *static_cast<const PhotoFile*>(left);
  const auto& b = *static_cast<const PhotoFile*>(right);
  return photo_policy::newestFirst(a.modified, a.path, b.modified, b.path);
}
int compareMedia(const void* left,const void* right) {
  const auto& a=*static_cast<const PhotoFile*>(left);
  const auto& b=*static_cast<const PhotoFile*>(right);
  if (a.captureOrder != b.captureOrder) return a.captureOrder > b.captureOrder ? -1 : 1;
  if(a.modified!=b.modified)return a.modified>b.modified?-1:1;
  return comparePhotos(left,right);
}
// The first sort only groups camera IDs newest first, exactly as when no
// dates were known yet; dates read with the listing must not affect it.
int compareInitial(const void* left, const void* right) {
  const auto& a = *static_cast<const PhotoFile*>(left);
  const auto& b = *static_cast<const PhotoFile*>(right);
  return photo_policy::newestFirst(0, a.path, 0, b.path);
}

// FatFs logical drive holding the SD card, or -1. The SD card is the only FAT
// volume on the device; a listing on it returns names and dates in one pass,
// instead of one stat (a directory scan) per file.
int fatDrive() {
  for (int drive = 0; drive < FF_VOLUMES; ++drive) {
    char root[4] = {char('0' + drive), ':', '/', 0};
    FF_DIR dir;
    if (f_opendir(&dir, root) == FR_OK) { f_closedir(&dir); return drive; }
  }
  return -1;
}

// Same conversion as ESP-IDF's FAT VFS stat(), so dates match fileInfo().
uint64_t fatTime(WORD date, WORD time) {
  struct tm tm = {};
  tm.tm_mday = date & 0x1f;
  tm.tm_mon = ((date >> 5) & 0xf) - 1;
  tm.tm_year = ((date >> 9) & 0x7f) + 80;
  tm.tm_sec = (time & 0x1f) * 2;
  tm.tm_min = (time >> 5) & 0x3f;
  tm.tm_hour = (time >> 11) & 0x1f;
  const time_t t = mktime(&tm);
  return t > 0 ? uint64_t(t) : 0;
}
}  // namespace

void PhotoCatalog::clear() {
  void* allocation = files_;
  core::memory::freePsram(allocation);
  files_ = nullptr;
  count_ = 0;
}

bool PhotoCatalog::removeAt(size_t index,bool fileRemoved) {
  if (index >= count_ || (!fileRemoved && !removeFile(files_[index].path))) return false;
  --count_;
  if (index < count_) {
    memmove(files_ + index, files_ + index + 1, (count_ - index) * sizeof(PhotoFile));
  }
  if (!count_) clear();
  return true;
}

PhotoCatalogResult PhotoCatalog::load(bool includeVideos) {
  clear(); timings_ = {};
  const uint32_t started = micros();
  if (!mounted()) return PhotoCatalogResult::NoSd;
  {
    SpiLock lock;
    size_t capacity=0;
    bool hasVideos=false;
    const char* folders[]={"/photos","/videos"};
    const int folderCount=includeVideos?2:1;
    const int drive=fatDrive();
    auto accepted=[](const char* name,bool directory,bool video) {
      const char* extension=strrchr(name,'.');
      return !directory && name[0]!='.' && (video ? extension && photo_policy::equal(extension,".avi") : photo_policy::isPhoto(name));
    };
    for (int pass=0;pass<2;++pass) {
      for (int folder=0;folder<folderCount;++folder) {
        auto add=[&](const char* name,bool directory,uint64_t modified)->int {  // 1 ok, 0 skip, -1 error
          if (!accepted(name,directory,folder==1)) return 0;
          if (!pass) {++capacity;return 0;}
          if (count_>=capacity) return -1;
          auto& entry=files_[count_++];
          const int n=snprintf(entry.path,sizeof(entry.path),"%s/%s",folders[folder],name);
          if (n<0 || size_t(n)>=sizeof(entry.path)) return -1;
          entry.modified=modified;entry.captureOrder=0;entry.video=folder==1;hasVideos=hasVideos||entry.video;
          return 1;
        };
        if (drive>=0) {
          char dirPath[24];
          snprintf(dirPath,sizeof(dirPath),"%d:%s",drive,folders[folder]);
          FF_DIR dir;
          const FRESULT opened=f_opendir(&dir,dirPath);
          if (opened==FR_NO_PATH || opened==FR_NO_FILE) continue;
          if (opened!=FR_OK) {clear();return PhotoCatalogResult::ReadError;}
          FILINFO info;
          bool failed=false;
          while (f_readdir(&dir,&info)==FR_OK && info.fname[0]) {
            if (add(info.fname,info.fattrib&AM_DIR,fatTime(info.fdate,info.ftime))<0) {failed=true;break;}
          }
          f_closedir(&dir);
          if (failed) {clear();return PhotoCatalogResult::ReadError;}
          continue;
        }
        if (!SD.exists(folders[folder])) continue;
        File dir=SD.open(folders[folder]);
        if (!dir || !dir.isDirectory()) {clear();return PhotoCatalogResult::ReadError;}
        bool directory=false;
        for (String path=dir.getNextFileName(&directory);path.length();path=dir.getNextFileName(&directory)) {
          if (add(basename(path.c_str()),directory,0)<0) {clear();return PhotoCatalogResult::ReadError;}
        }
      }
      if (!pass) {
        if (!capacity) return PhotoCatalogResult::Ok;
        if (capacity>SIZE_MAX/sizeof(PhotoFile)) return PhotoCatalogResult::NoMemory;
        const uint32_t at=micros();
        files_=static_cast<PhotoFile*>(core::memory::allocPsram("photos.catalog",capacity*sizeof(PhotoFile)));
        timings_.allocationUs=micros()-at;
        if (!files_) return PhotoCatalogResult::NoMemory;
      }
    }
    if (count_!=capacity) {clear();return PhotoCatalogResult::ReadError;}
    qsort(files_,count_,sizeof(PhotoFile),compareInitial);
    for (size_t first=0;first<count_;) {
      const uint64_t sequence=photo_policy::sequence(files_[first].path);
      size_t end=first+1;
      while (end<count_ && photo_policy::sequence(files_[end].path)==sequence) ++end;
      // Only actual mixed media needs timestamps for every camera image.
      // A photo-only library stays newest-first by monotonic capture ID even
      // before clock sync, and avoids opening every JPEG just for metadata.
      const bool needed=hasVideos || !sequence || end-first>1;
      for (size_t index=first;index<end;++index) {
        if (!needed) {files_[index].modified=0;continue;}   // as if never read
        if (drive>=0) continue;                              // dated by the listing
        size_t bytes=0;
        uint32_t modified=0;
        if (!fileInfo(files_[index].path,bytes,modified)) {clear();return PhotoCatalogResult::ReadError;}
        files_[index].modified=modified>0?uint64_t(modified):0;
        ++timings_.metadataReads;
      }
      first=end;
    }
    if (hasVideos) {
      // The initial sequence sort puts camera images newest-first. Walk their
      // IDs oldest-to-newest and prevent a clock reset from dating a new
      // capture before an older one. Only the in-memory sort key is adjusted;
      // original files and imported/video timestamps remain untouched.
      uint64_t previous=0;
      for (size_t i=count_;i>0;--i) {
        auto& entry=files_[i-1];
        if (entry.video || !photo_policy::sequence(entry.path)) continue;
        if (entry.modified<previous) entry.modified=previous;
        else previous=entry.modified;
      }
    }
    // Legacy clips have random names. Keep their existing date order, and
    // sort captures from the first numbered clip onward by their shared ID.
    uint64_t firstClip = UINT64_MAX;
    for (size_t i = 0; i < count_ && hasVideos; ++i) {
      const auto& entry = files_[i];
      const uint64_t id = photo_policy::videoSequence(basename(entry.path));
      if (id && id < firstClip) firstClip = id;
    }
    if (firstClip != UINT64_MAX) {
      for (size_t i = 0; i < count_; ++i) {
        auto& entry = files_[i];
        const uint64_t id = entry.video ? photo_policy::videoSequence(basename(entry.path)) :
                                         photo_policy::sequence(entry.path);
        if (id >= firstClip && id <= UINT32_MAX) entry.captureOrder = static_cast<uint32_t>(id);
      }
    }
    qsort(files_,count_,sizeof(PhotoFile),hasVideos?compareMedia:comparePhotos);
  }
  timings_.totalUs=micros()-started;
  return PhotoCatalogResult::Ok;
}

} // namespace sd
} // namespace hal
