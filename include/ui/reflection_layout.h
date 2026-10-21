#pragma once
#include "ui/safe_area_layout.h"

namespace ui { namespace reflection {
using safe_area::Box;
struct Layout { Box avatar, eyes, dialogue, skip, talk, text; };
constexpr Layout inColumn(Box column, int buttonY, int buttonW) {
  return {
    {column.x+4,40,24,26}, {column.x+8,47,16,10},
    {column.x,column.y,column.w,buttonY-10-column.y},
    {column.x,buttonY,buttonW,34},
    {column.x+buttonW+8,buttonY,column.w-buttonW-8,34},
    {column.x+16,column.y+10,column.w-32,buttonY-10-column.y-20}
  };
}
constexpr Layout layout(int width=280, int height=240) {
  // Fit the entire column to the lowest control, then reuse its exact rails.
  // Raising the bottom edge avoids narrowing only the buttons at the bezel.
  return inColumn(safe_area::fitBox({22,78,width-44,height-12-78}),height-46,
      (safe_area::fitBox({22,78,width-44,height-12-78}).w-8)/2);
}
}}
