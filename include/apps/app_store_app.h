#pragma once
#include "app.h"
namespace apps { namespace app_store {
extern App app;
enum class TutorialResult { None, Back, Skipped, Installed };
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool submitStoreCode(const char*);
const char* installDiagnostic();
#endif
void beginTutorial();
TutorialResult takeTutorialResult();
} }
