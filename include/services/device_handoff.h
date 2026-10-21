#pragma once
namespace services { namespace device_handoff {
// UI task after foreground teardown. Quiesces background owners, preserves
// firmware asset roots/calibration/OTA key, removes personal SD and NVS data,
// verifies retained assets and deleted entries, then reboots even on partial failure.
// Logical deletion, not forensic sanitization of flash/SD wear-levelled cells.
bool scrubAndRestart();
void printStatus();
} }
