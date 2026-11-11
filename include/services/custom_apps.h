#pragma once
#include "hal/app_storage.h"
#include <stddef.h>
#include <stdint.h>

namespace services { namespace custom_apps {
using Medium = hal::app_storage::Medium;
using Cancel = hal::app_storage::Cancel;
constexpr size_t kScriptBytes=32768, kDataBytes=8192, kManifestBytes=1024;
#ifdef ORIGAMI_NATIVE_APPS
constexpr size_t kPackageBytes=12+kManifestBytes+512*1024;
#else
constexpr size_t kPackageBytes=12+kManifestBytes+kScriptBytes+kDataBytes;
#endif
constexpr size_t kCatalogEntries=16;
enum class Result : uint8_t { Ok, Missing, Unavailable, Corrupt, Invalid, Incompatible,
    NoMemory, NoSpace, Io, Cancelled, Uncertain, Busy, Network };
const char* resultName(Result);
// Fixed owned identity: origin is "local", or a 64-hex backend/account digest.
// The adapter assigns origin, never the package. Legacy entries are read-only SD.
struct Identity { Medium medium=Medium::Sd; char origin[65]="local"; char id[32]={}; bool legacy=false; };
struct Entry { Identity identity; char name[49]={}; bool native=false,hasIcon=false; uint8_t icon[50]={}; };
struct Catalog { Entry entries[kCatalogEntries]; size_t count=0; bool overflow=false; };
// Move-free owner. Source/data pointers remain valid until reset/destruction.
// An app keeps one complete snapshot across overlay reloads, so file reclamation
// cannot alter a mounted app. No filesystem handle or storage lease escapes.
struct Package {
    Package()=default; ~Package();
    Package(const Package&)=delete; Package& operator=(const Package&)=delete;
    uint8_t* bytes=nullptr; size_t size=0;
    void reset();
};
struct View { bool native=false,hasIcon=false; uint8_t icon[50]={}; char id[32]={},name[49]={},version[32]={};
    const char* source=nullptr; size_t sourceBytes=0;
    const char* data=nullptr; size_t dataBytes=0; };
bool validId(const char*);
bool validIdentity(const Identity&);
// Package wire format: KAPP + LE32(schema=1) + LE32(manifest bytes), JSON manifest,
// exact source bytes, exact optional data bytes. Text-only Lua, no embedded NUL.
// Validates schema, lengths, SHA256 and data JSON; NEVER executes Lua.
Result inspect(const uint8_t*,size_t,View&);
// Build a package from bounded owned-by-caller inputs (cloud/legacy adapter).
Result pack(const char* id,const char* name,const char* version,const char* source,
            size_t sourceBytes,const char* data,size_t dataBytes,Package& out);
// All filesystem functions below block and are SINGLE existing widget worker
// only. The composition owner serializes work/admission. Inputs borrowed for the
// duration of the call; output packages/catalogs are owned. No callbacks upward.
Result load(const Identity&,Package&);
Result install(const Identity&,const uint8_t*,size_t,Cancel=nullptr);
Result catalog(Catalog&);
// Uninstall publishes a tombstone, preserves data/, and reclaims only known code
// slots. Explicit removal also recovers corrupt records and incompatible code.
// Cancellation after activation cannot turn a committed change into cancel.
Result uninstall(const Identity&,Cancel=nullptr);
// Cloud namespace digest of URL + account key. Stored digest contains no key;
// caller must also reject stale configuration completions before publishing.
bool accountOrigin(const char* url,const char* key,char (&out)[65]);
} }
