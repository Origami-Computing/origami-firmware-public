#pragma once
#ifdef ORIGAMI_DEV_SERIAL_INPUT
namespace apps { namespace screen_brightness_test {
// Development-only optical reference. No NVS writes or media operations.
// drive=-1 uses the current production curve; 0..100 tests an uncalibrated drive.
// Refuses duplicate activation while covered. Back/C/chord/15s timeout closes it.
bool show(int target, int drive, bool swap);
bool close();
} }
#endif
