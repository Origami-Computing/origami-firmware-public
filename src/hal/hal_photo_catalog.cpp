#include "hal/hal_photo_catalog.h"

#include <SD.h>
#include <stdlib.h>
#include <string.h>

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
    auto accepted=[](const String& path,bool directory,bool video) {
      const char* name=basename(path.c_str());
      const char* extension=strrchr(name,'.');
      return !directory && name[0]!='.' && (video ? extension && photo_policy::equal(extension,".avi") : photo_policy::isPhoto(name));
    };
    for (int pass=0;pass<2;++pass) {
      for (int folder=0;folder<folderCount;++folder) {
        if (!SD.exists(folders[folder])) continue;
        File dir=SD.open(folders[folder]);
        if (!dir || !dir.isDirectory()) {clear();return PhotoCatalogResult::ReadError;}
        bool directory=false;
        for (String path=dir.getNextFileName(&directory);path.length();path=dir.getNextFileName(&directory)) {
          if (!accepted(path,directory,folder==1)) continue;
          if (!pass) {++capacity;continue;}
          if (count_>=capacity) {clear();return PhotoCatalogResult::ReadError;}
          auto& entry=files_[count_++];
          const int n=snprintf(entry.path,sizeof(entry.path),"%s/%s",folders[folder],basename(path.c_str()));
          if (n<0 || size_t(n)>=sizeof(entry.path)) {clear();return PhotoCatalogResult::ReadError;}
          entry.modified=0;entry.captureOrder=0;entry.video=folder==1;hasVideos=hasVideos||entry.video;
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
    qsort(files_,count_,sizeof(PhotoFile),hasVideos?compareMedia:comparePhotos);
    for (size_t first=0;first<count_;) {
      const uint64_t sequence=photo_policy::sequence(files_[first].path);
      size_t end=first+1;
      while (end<count_ && photo_policy::sequence(files_[end].path)==sequence) ++end;
      // Only actual mixed media needs timestamps for every camera image.
      // A photo-only library stays newest-first by monotonic capture ID even
      // before clock sync, and avoids opening every JPEG just for metadata.
      if (hasVideos || !sequence || end-first>1) for (size_t index=first;index<end;++index) {
        File entry=SD.open(files_[index].path,FILE_READ);
        if (!entry) {clear();return PhotoCatalogResult::ReadError;}
        const time_t modified=entry.getLastWrite();
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
