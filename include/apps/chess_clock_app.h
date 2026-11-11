#pragma once
#include "app.h"
namespace apps { namespace chess_clock {
// Wheel selects time control before play; C starts/pauses; A/B finish that
// player's turn. Cover pauses; resume requires C. A+B uses normal navigation.
extern App app;
}}
