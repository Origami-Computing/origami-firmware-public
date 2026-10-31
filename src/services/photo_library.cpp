#include "services/photo_library.h"
#include "services/agent_coordination.h"
#include "services/uploads.h"
#include "services/media_delete.h"
#include "services/background_network.h"
#include "hal/hal_wifi.h"
#include <string.h>
namespace services { namespace photos {
Library::BrowseResult Library::beginBrowsing(bool deferred) {
    active_=true;
    browseReady_=false;
    const auto admission=deferred ? reservation_.pollAcquire(false) : reservation_.acquire(false,8000);
    if (admission==background_network::PeripheralReservation::Result::Pending) return BrowseResult::Pending;
    if (admission!=background_network::PeripheralReservation::Result::Acquired)
        return BrowseResult::UploadsBusy;
    if (!(deferred ? browseAgentPause_.pollAcquire() : browseAgentPause_.acquire(8000))) {
        if (deferred) return BrowseResult::Pending;
        reservation_.release();
        return BrowseResult::NetworkBusy;
    }
    hal::wifi::stop();
    browseAgentPause_.release();
    browseReady_=true;
    return BrowseResult::Ready;
}
Library::LoadResult Library::load() {
    return browsing() ? catalog_.load(true) : LoadResult::ReadError;
}
bool Library::removeAt(size_t index,int* removedUploads) {
    if (removedUploads) *removedUploads=0;
    if (!browsing() || index>=count()) return false;
    char path[sizeof(File::path)];
    memcpy(path,catalog_[index].path,sizeof(path));
    path[sizeof(path)-1]=0;
    if(catalog_[index].video){
        media_delete::Result result;
        if(!media_delete::deleteFile(path,&result))return false;
        if(removedUploads)*removedUploads=result.queueRemoved;
        return catalog_.removeAt(index,true);
    }
    if (!catalog_.removeAt(index)) return false;
    int removed=0;
    while (uploads::queue().removeByFilePath(path)) ++removed;
    if (removedUploads) *removedUploads=removed;
    return true;
}
int Library::beginSync() {
    browseReady_=false;
    browseAgentPause_.release();
    active_=true;
    (void)uploads::ensureInit();
    auto& queue=uploads::queue();
    queue.setUploadPaused(false);
    const int pending=queue.counts().pending;
    hal::wifi::scheduleRestore();
    reservation_.release();
    return pending;
}
Library::SyncStatus Library::syncStatus() const {
    auto& queue=uploads::queue();
    const auto counts=queue.counts();
    return {counts.pending,counts.failed,hal::wifi::connected()};
}
void Library::close() {
    browseReady_=false;
    browseAgentPause_.release();
    catalog_.clear();
    if (!active_) return;
    active_=false;
    hal::wifi::scheduleRestore();
    reservation_.release();
}
} }
