#pragma once
#include "hal/hal_photo_catalog.h"
#include "services/peripheral_reservation.h"

namespace services { namespace photos {
// UI-task gallery operations. The app owns selection, menus and sync-view timing.
// The library owns catalog storage and browsing/network exclusion. No rendering.
class Library {
public:
    enum class BrowseResult { Ready, UploadsBusy, NetworkBusy, Pending };
    using LoadResult = hal::sd::PhotoCatalogResult;
    using File = hal::sd::PhotoFile;
    struct SyncStatus { int pending; int failed; bool connected; };
    Library() = default;
    ~Library() { close(); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    // deferred retains a Pending handoff for subsequent calls; close cancels.
    BrowseResult beginBrowsing(bool deferred=false);
    bool browsing() const { return browseReady_; }
    // Requires successful beginBrowsing(); entries remain valid until load,
    // clear, successful removal or close. Indices must be less than count().
    LoadResult load();
    void clear() { catalog_.clear(); }
    size_t count() const { return catalog_.count(); }
    const File& operator[](size_t index) const { return catalog_[index]; }
    const hal::sd::PhotoCatalogTimings& timings() const { return catalog_.timings(); }
    // SD removal must succeed before pending queue entries are removed.
    // Never deletes a cloud copy. Failure leaves queued uploads unchanged.
    bool removeAt(size_t index, int* removedUploads);
    int beginSync(); // Returns initial pending count; preserves the catalog.
    SyncStatus syncStatus() const;
    void close();
private:
    hal::sd::PhotoCatalog catalog_;
    background_network::PeripheralReservation reservation_;
    agent_coordination::TransportPause browseAgentPause_;
    bool active_=false, browseReady_=false;
};
} }
