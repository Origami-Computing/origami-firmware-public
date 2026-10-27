#pragma once
#include <TB_TFT_eSPI.h>
#include "ui/system_status_bar.h"
namespace ui {
// UI-task-owned PSRAM caches for bounded tutorial motion. App owns timing;
// renderer owns wheel frames and closing-frame/wave composition and cleanup.
class OnboardingMotion {
public:
 OnboardingMotion();~OnboardingMotion();
 bool prepareOutro();
 bool prepareWheel();
 bool drawWheel(TFT_eSprite& target,unsigned elapsed);
 void close();
 bool drawOutro(TFT_eSprite& target,TFT_eSprite& closing,unsigned elapsed,SystemStatusBar& chrome);
private:
 TFT_eSprite frame_;
 uint16_t* wheel_=nullptr;
};
}
