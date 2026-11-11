#pragma once
#include <stdint.h>
#include <math.h>

// Output-only RGB565 transform. Source sprites, captures and image buffers stay
// unchanged. Three small tables avoid floating-point work in the transfer loop.
// UI task owns updates; the display SPI lock excludes transfers during set().
class PixelScale565 {
 public:
  explicit PixelScale565(unsigned percent = 100) { set(percent); }
  void set(unsigned percent) {
    percent_ = percent > 100 ? 100 : percent;
    // Treat the percentage as CIE L* lightness of white. Decode/encode sRGB so
    // scaling happens in linear light, preserving chromaticity before rounding.
    const float lightness = percent_;
    const float t = (lightness + 16.0f) / 116.0f;
    const float luminance = lightness <= 8.0f ? lightness / 903.2963f : t*t*t;
    for (unsigned i = 0; i < 32; ++i) {
      const unsigned value = channel(i, 31, luminance);
      red_[i] = value << 11;
      blue_[i] = value;
      fineRB_[i] = static_cast<unsigned>(encoded(i,31,luminance)*31*256+.5f);
    }
    for (unsigned i = 0; i < 64; ++i) {
      green_[i] = channel(i, 63, luminance) << 5;
      fineG_[i] = static_cast<unsigned>(encoded(i,63,luminance)*63*256+.5f);
    }
  }
  unsigned percent() const { return percent_; }
  bool active() const { return percent_ != 100; }
  uint16_t map(uint16_t rgb) const {
    return red_[rgb >> 11] | green_[(rgb >> 5) & 63] | blue_[rgb & 31];
  }
  // Stable spatial rounding for photographs: retain fractional output shades
  // instead of collapsing large source ranges into a few colored contours.
  // The same threshold for all channels keeps neutral noise correlated.
  uint16_t mapDithered(uint16_t rgb, unsigned x, unsigned y) const {
    if (!active()) return rgb;
    static constexpr unsigned char bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    const unsigned threshold=bayer[(y&3)*4+(x&3)]*16+8;
    const auto quantize=[threshold](unsigned value) {
      return (value>>8)+((value&255)>threshold);
    };
    return (quantize(fineRB_[rgb>>11])<<11) |
           (quantize(fineG_[(rgb>>5)&63])<<5) | quantize(fineRB_[rgb&31]);
  }
 private:
  float encoded(unsigned code, unsigned maximum, float luminance) const {
    if (percent_ == 100) return static_cast<float>(code)/maximum;
    if (percent_ == 0) return 0;
    const float value = static_cast<float>(code) / maximum;
    const float linear = value <= .04045f ? value / 12.92f : powf((value+.055f)/1.055f, 2.4f);
    const float dimmed = linear * luminance;
    return dimmed <= .0031308f ? 12.92f*dimmed : 1.055f*powf(dimmed, 1.0f/2.4f)-.055f;
  }
  unsigned channel(unsigned code, unsigned maximum, float luminance) const {
    if (percent_ == 100) return code;
    return static_cast<unsigned>(encoded(code,maximum,luminance)*maximum+.5f);
  }
  unsigned percent_ = 100;
  uint16_t red_[32], green_[64], blue_[32];
  uint16_t fineRB_[32], fineG_[64];
};
