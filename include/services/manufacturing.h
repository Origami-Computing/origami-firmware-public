#pragma once
#include <stdint.h>
namespace services { namespace manufacturing {
constexpr unsigned kChecks = 16;
constexpr uint32_t kAll = (1u << kChecks) - 1;
// UI-task owner of atomic NVS result and override records, bound to the running ELF.
// No credentials, user content, network names or operator identities are stored.
bool begin();
uint32_t manualSteps();
bool setManualStep(unsigned step, bool complete);
bool manualStep(unsigned step);
bool result(unsigned check, bool passed);
bool invalidate(unsigned check);
bool accepted();
bool report(uint32_t& passed, uint32_t& failed);
void printStatus();
bool printImageHash(uint32_t bytes);
bool verifyImage(uint32_t bytes, const char* expected);
bool markOnline(bool production);
bool markClean();
bool markUnlock();
bool resetTutorial();
bool tutorialReady();
// Test arming requires a persisted, image-bound bench marker; ordinary arming rejects it.
bool markBench();
bool readyToArm(bool testMode=false, bool candidateMode=false);
// Read-only inventory; releases the shared bus between entries. Cleanup callers
// must first quiesce writers when they require a stable empty-card assertion.
void printInventory();
} }
