#pragma once
#include <stddef.h>
#include <stdint.h>
namespace services { namespace native_apps {
constexpr size_t kMaxBytes = 512 * 1024;
// Worker-safe publisher authentication. Inputs borrowed synchronously. Does not
// execute code. Signature authenticates identity, version, ABI and ELF digest.
bool verify(const char* id,const char* name,const char* version,unsigned abi,
            const char* digest,const char* signature,const char* icon=nullptr);
// Existing storage worker: bounded relocation of authenticated immutable bytes.
// Does not execute module code. One module at a time; busy returns false.
bool prepare(const uint8_t* elf,size_t bytes);
// UI task after storage completion has transferred ownership. Runs constructors
// and entry once, returning a module-owned descriptor valid until collection.
bool activate(void*& descriptor);
// Worker/UI cancellation before activation; never destroys an active module.
void discardPrepared();
// UI task after app teardown: mark for later collection, never free while a
// module callback may still be returning through navigation.
void retire();
// UI-loop boundary only, outside all app/screen/input callback stacks.
void collect();
} }
