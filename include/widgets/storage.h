#pragma once
#include "services/custom_apps.h"
#include <stdint.h>

namespace widgets { namespace storage {
using services::custom_apps::Identity;
using services::custom_apps::Package;
using services::custom_apps::Catalog;
using services::custom_apps::Result;
enum class Operation { Catalog, Load, Install, Import, Remove, InitializeFlash, Sync, Download, PrepareNative };
// UI task only. Returns a nonzero operation ticket or zero if busy/unavailable.
// Submission owns a copy of package bytes. No storage/network I/O on UI task.
uint32_t request(Operation,const Identity& = Identity{},const uint8_t* bytes=nullptr,size_t size=0,
                 const char* backendId=nullptr,const char* slug=nullptr);
// UI task: queue a public HTTPS KAPP download from a hash-pinned app-store QR.
// Uses the same worker, ticket, cancellation and persistence owner as installs.
// Temporary worker preemption retries the owned request; explicit cancel/abandon
// remains terminal. A retry never replaces a successfully committed outcome.
enum class DownloadTarget { ByKind, Sd, Flash };
// ByKind places Lua on SD and native modules in persistent internal flash.
uint32_t requestDownload(const char* qr, DownloadTarget target=DownloadTarget::ByKind);
// UI task: take terminal result once; transfers owned snapshot to package.
// Catalog output optional. Outputs untouched until completion. No blocking.
bool take(uint32_t ticket,Result& result,Package* package=nullptr,Catalog* catalog=nullptr,Identity* identity=nullptr);
struct DownloadProgress {
    enum class Phase { Preparing, Downloading, Installing };
    DownloadProgress(Phase stage=Phase::Preparing,int value=-1):phase(stage),percent(value){}
    Phase phase;
    int percent; // Unknown Content-Length: show activity, never invent a percentage.
};
// UI task: coherent snapshot for this ticket; stale/retired tickets return Preparing.
DownloadProgress downloadProgress(uint32_t ticket);
// Widget worker only, synchronous byte-count publication; no retained pointers.
void reportDownloadProgress(size_t received,size_t total);
void cancel(uint32_t ticket); // request cancel; take still returns the actual terminal outcome
void abandon(uint32_t ticket); // request cancel, retire completion; worker owns cleanup
// Existing widget_poll only, after normal admission and outside HTTP batch.
bool pending();
bool networkPending(); // worker: queued sync needs existing TLS/reconnect admission
bool cancelRequested(); // worker cancellation checkpoint for queued sync
void process(bool (*pausePending)());
// Worker-only validation: syntax compile in a capped private VM; no bindings or
// top-level execution, so validation cannot perform device operations.
Result validateLua(const uint8_t*,size_t);
} }
