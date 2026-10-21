#pragma once
#include <TB_TFT_eSPI.h>
#include "ui/eye_rig.h"
namespace ui { namespace kami_dialogue {
#ifdef ORIGAMI_INSTRUMENT_FONT
constexpr int kLineH=26;
#else
constexpr int kLineH=22;
#endif
void drawAvatar(TFT_eSPI& target, int x, int y, bool eyesOpen=true, const char* emotion="neutral");
void drawAvatarPose(TFT_eSPI& target,int x,int y,bool eyesOpen,const eye_rig::Pose& pose,
                    float spacing=eye_rig::kDefaultSpacing,const kami_expression::Frame* expression=nullptr);
}}
