#pragma once

namespace ui { namespace layout {
// Shared 4 px spacing scale. Use space(n) for gaps, padding and new layout rails.
constexpr int space(int units) { return units * 4; }

// Default list geometry. Text uses its font metrics inside these grid boxes.
constexpr int kListRowH = 35;
constexpr int kListRowPitch = 37;
constexpr int kListTopPad = 2;
constexpr int kListLeftInset = 14;
constexpr int kListScrollbarW = 3;
constexpr int kListScrollbarGap = 5;
constexpr int kListRightInset = 7;
constexpr int kListTextInset = space(1);
constexpr int kListIndicatorW = space(4);
constexpr int kListIndicatorInset = space(1);
constexpr int kListTextGap = space(1);

// Existing flow-widget geometry, shared by Painter and scene::Builder.
// Consolidation preserves current font/chrome sizing and screen composition.
constexpr int kRowH = space(7);
constexpr int kPanelH = 22;
constexpr int kStatusLineH = 18;
constexpr int kLabelH = 18;
constexpr int kBigNumberH = space(19);
constexpr int kProgressH = space(2);
constexpr int kProgressPadH = space(3);
constexpr int kClockH = space(13);
constexpr int kOnOffRowH = space(7);
constexpr int kFooter1H = 22;
constexpr int kFooter2H = 30;
constexpr int kFooter3H = space(11);
} }
