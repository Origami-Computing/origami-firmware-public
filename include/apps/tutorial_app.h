#pragma once

#include "app.h"

namespace apps { namespace tutorial {
extern App app;
// Called by the UI task. Offers once per boot, after the boot animation exits.
void serviceBootOffer();
bool isActive();
// Remains true while a real scanner/menu is above the tutorial on the stack.
bool sessionActive();
bool navigationLessonActive();
bool navigationPracticed();
void noteNavigation();
// UI-task return from a menu/app practice chord, preserving tutorial ownership.
bool returnFromNavigation();
// Explicit user replay: open the first lesson without clearing saved settings.
void replay();
// Reuse just the Wi-Fi lessons, returning to the caller without saving progress.
void beginWifiSetup();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Developer-only controls for repeatable first-boot and persistence checks.
bool restartForTesting();
void printStatus();
bool debugSeek(unsigned page,unsigned ms);
void debugPlay();
#endif
}} // namespace apps::tutorial
