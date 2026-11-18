#pragma once
// Loudness metering after ITU-R BS.1770-4: K-weighting, 400 ms blocks with 75% overlap, absolute
// gate -70 LUFS and relative gate -10 LU. Channel weights: 1.0 for front and height channels, 1.41
// (+1.5 dB) for surround channels, LFE excluded. Also momentary (400 ms) and short-term (3 s)
// loudness and an approximate true peak (4x oversampled). Real-time safe: no allocation after
// construction.
#include <array>
#include <cstddef>
#include <vector>

#include "immersive/layout.h"

namespace wezt::immersive {

class LoudnessMeter {
 public:
  LoudnessMeter(double sampleRate, const SpeakerLayout& layout);
  void reset();
  // Planar audio, one pointer per layout channel.
  void feed(const float* const* planar, std::size_t frames);

  float momentary() const;      // LUFS, -inf (-200) when not enough audio yet
  float shortTerm() const;
  float integrated() const;     // gated
  float truePeakDb() const { return truePeakDb_; }   // dBTP since reset
  float samplePeakDb() const;

 private:
  struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
  static constexpr std::size_t kHistBins = 1700;  // -70 .. +15 LUFS in 0.05 LU steps
  void finishStep();
  double rate_;
  std::size_t channels_;
  std::vector<double> weight_;
  std::vector<std::array<Biquad, 2>> filt_;
  std::vector<double> sumSq_;
  std::size_t stepLen_, stepPos_ = 0;
  std::array<double, 30> steps_{};  // weighted mean-square per 100 ms step, newest at (stepCount_-1)%30
  std::size_t stepCount_ = 0;
  std::vector<unsigned> hist_;
  std::vector<std::array<float, 12>> tpHist_;
  std::vector<std::size_t> tpPos_;
  float truePeakDb_ = -200.f, samplePeak_ = 0.f;
};

}  // namespace wezt::immersive
