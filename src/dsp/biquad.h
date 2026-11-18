#pragma once
// Biquad filters after the public RBJ "Audio EQ Cookbook" formulas, in double precision.
#include <cmath>

namespace wezt::dsp {

enum class FilterType { Peaking, LowShelf, HighShelf, HighPass, LowPass };

struct BiquadCoeffs {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  static BiquadCoeffs design(FilterType t, double sampleRate, double freq, double gainDb, double q) {
    constexpr double kPi = 3.14159265358979323846;
    BiquadCoeffs c;
    if (sampleRate <= 0) return c;
    freq = std::fmin(std::fmax(freq, 10.0), sampleRate * 0.49);
    q = std::fmax(q, 0.05);
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * freq / sampleRate, cs = std::cos(w), sn = std::sin(w), al = sn / (2 * q);
    double b0, b1, b2, a0, a1, a2;
    switch (t) {
      case FilterType::Peaking:
        b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; break;
      case FilterType::LowShelf: {
        const double s = 2 * std::sqrt(A) * al;
        b0 = A * ((A + 1) - (A - 1) * cs + s); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - s);
        a0 = (A + 1) + (A - 1) * cs + s; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - s; break;
      }
      case FilterType::HighShelf: {
        const double s = 2 * std::sqrt(A) * al;
        b0 = A * ((A + 1) + (A - 1) * cs + s); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - s);
        a0 = (A + 1) - (A - 1) * cs + s; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - s; break;
      }
      case FilterType::HighPass:
        b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; break;
      default:
        b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; break;
    }
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
  }
  // Magnitude response in dB at `freq`.
  double responseDb(double sampleRate, double freq) const {
    constexpr double kPi = 3.14159265358979323846;
    const double w = 2 * kPi * freq / sampleRate, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
  }
};

struct Biquad {
  BiquadCoeffs c;
  double z1 = 0, z2 = 0;
  float run(float xf) {  // transposed direct form II
    const double x = xf, y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return static_cast<float>(y);
  }
  void reset() { z1 = z2 = 0; }
};

}  // namespace wezt::dsp
