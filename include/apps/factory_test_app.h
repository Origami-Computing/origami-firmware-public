#pragma once
#include "app.h"
namespace apps { namespace factory_test {
extern App app;
// UI-task boot gate, before tutorial offers. True while factory gate owns boot.
bool serviceBootOffer();
bool requestServiceAccess();
void restart();
void printStatus();
// UI-task station adapter. stationRecord stores an explicit operator judgment.
bool stationRun(unsigned check);
bool stationConfirm(bool passed);
bool stationRecord(unsigned check,bool passed);
bool stationAudio(bool record);
} }
