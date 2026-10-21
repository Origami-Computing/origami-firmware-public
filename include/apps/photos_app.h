#pragma once
#include "app.h"

class Stream;

namespace apps {
namespace photos {
extern App app;
void dumpStatus(Stream& output);
}  // namespace photos
}  // namespace apps
