#pragma once
namespace control { namespace native_apps {
// UI-task composition after the storage ticket has transferred a prepared module.
// Mounts the original App in NavHandler; failure retires it for collection.
bool openPrepared();
// Call at the top of the UI loop, never from a callback into module code.
void collect();
} }
