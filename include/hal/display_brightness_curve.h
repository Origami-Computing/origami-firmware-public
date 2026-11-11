#pragma once

namespace hal { namespace display {
// Current rev-4 LCD profile, measured against same-frame optical white/black
// coverage on COM14. Values are the generic scaler's CIE L* drive, not UI %.
// 25/40/50/75/90% matched at drives 27/50/59/79/88; preserve the low-end analytical
// curve where the webcam's sparse-pattern accuracy is limited. Full brightness
// remains exactly unchanged. See the screen-brightness verification evidence.
inline unsigned calibratedBrightnessDrive(unsigned percent) {
  struct Knot { unsigned target, drive; };
  static constexpr Knot points[]={{0,0},{10,10},{25,27},{40,50},{50,59},{75,79},{90,88},{100,100}};
  if(percent>=100)return 100;
  for(unsigned i=1;i<sizeof(points)/sizeof(points[0]);++i) {
    if(percent<=points[i].target) {
      const unsigned width=points[i].target-points[i-1].target;
      const unsigned rise=points[i].drive-points[i-1].drive;
      return points[i-1].drive+((percent-points[i-1].target)*rise+width/2)/width;
    }
  }
  return 100;
}
} }
