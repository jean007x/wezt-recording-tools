#include "immersive/loudness.h"

#include <algorithm>
#include <cmath>

namespace wezt::immersive {
namespace {
constexpr double kPi = 3.14159265358979323846;
double lufsOf(double z) { return z > 0.0 ? -0.691 + 10.0 * std::log10(z) : -200.0; }
// 4x interpolation filter: windowed sinc, 12 taps per phase (6 each side).
const std::array<std::array<float, 12>, 3>& phases() {
  static const auto t = [] {
    std::array<std::array<float, 12>, 3> p{};
    for (int ph = 1; ph <= 3; ++ph) {
      const double frac = ph / 4.0;
      double sum = 0.0;
      for (int k = 0; k < 12; ++k) {
        const double x = (k - 5) - frac;  // tap k sits at integer offset k-5 relative to the sample before the new point
        const double s = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double w = 0.5 + 0.5 * std::cos(kPi * x / 6.0);
        p[static_cast<std::size_t>(ph - 1)][static_cast<std::size_t>(k)] = static_cast<float>(s * w);
        sum += s * w;
      }
      for (auto& v : p[static_cast<std::size_t>(ph - 1)]) v = static_cast<float>(v / sum);
    }
    return p;
  }();
  return t;
}
}  // namespace

LoudnessMeter::LoudnessMeter(double sampleRate, const SpeakerLayout& layout)
    : rate_(sampleRate), channels_(layout.size()), weight_(layout.size(), 1.0), filt_(layout.size()), sumSq_(layout.size(), 0.0),
      stepLen_(static_cast<std::size_t>(std::lround(sampleRate * 0.1))), hist_(kHistBins, 0), tpHist_(layout.size()), tpPos_(layout.size(), 0) {
  for (std::size_t i = 0; i < channels_; ++i) {
    const Speaker& s = layout.speakers[i];
    if (s.lfe) weight_[i] = 0.0;
    else if (s.elevation < 1.f && std::fabs(s.azimuth) > 100.f) weight_[i] = 1.41;  // surround channels
  }
  // K-weighting: high shelf (head model) followed by the RLB high-pass, designed for any sample rate.
  double K = std::tan(kPi * 1681.974450955533 / rate_);
  const double Vh = std::pow(10.0, 3.999843853973347 / 20.0), Vb = std::pow(Vh, 0.4996667741545416), Q1 = 0.7071752369554196;
  const double a0 = 1.0 + K / Q1 + K * K;
  Biquad shelf;
  shelf.b0 = (Vh + Vb * K / Q1 + K * K) / a0;
  shelf.b1 = 2.0 * (K * K - Vh) / a0;
  shelf.b2 = (Vh - Vb * K / Q1 + K * K) / a0;
  shelf.a1 = 2.0 * (K * K - 1.0) / a0;
  shelf.a2 = (1.0 - K / Q1 + K * K) / a0;
  K = std::tan(kPi * 38.13547087602444 / rate_);
  const double Q2 = 0.5003270373238773, a0b = 1.0 + K / Q2 + K * K;
  Biquad hp;
  hp.b0 = 1.0; hp.b1 = -2.0; hp.b2 = 1.0;
  hp.a1 = 2.0 * (K * K - 1.0) / a0b;
  hp.a2 = (1.0 - K / Q2 + K * K) / a0b;
  for (auto& f : filt_) { f[0] = shelf; f[1] = hp; }
  reset();
}

void LoudnessMeter::reset() {
  for (auto& f : filt_) for (auto& b : f) { b.z1 = b.z2 = 0.0; }
  std::fill(sumSq_.begin(), sumSq_.end(), 0.0);
  stepPos_ = 0; stepCount_ = 0;
  steps_.fill(0.0);
  std::fill(hist_.begin(), hist_.end(), 0u);
  for (auto& h : tpHist_) h.fill(0.f);
  std::fill(tpPos_.begin(), tpPos_.end(), 0);
  truePeakDb_ = -200.f; samplePeak_ = 0.f;
}

void LoudnessMeter::finishStep() {
  double z = 0.0;
  for (std::size_t c = 0; c < channels_; ++c) { z += weight_[c] * sumSq_[c] / static_cast<double>(stepLen_); sumSq_[c] = 0.0; }
  steps_[stepCount_ % steps_.size()] = z;
  ++stepCount_;
  stepPos_ = 0;
  if (stepCount_ >= 4) {
    double blk = 0.0;
    for (std::size_t k = 0; k < 4; ++k) blk += steps_[(stepCount_ - 1 - k) % steps_.size()];
    blk /= 4.0;
    const double l = lufsOf(blk);
    if (l > -70.0) {
      const auto bin = static_cast<std::size_t>(std::min<double>(static_cast<double>(kHistBins - 1), (l + 70.0) / 0.05));
      ++hist_[bin];
    }
  }
}

void LoudnessMeter::feed(const float* const* planar, std::size_t frames) {
  for (std::size_t i = 0; i < frames; ++i) {
    for (std::size_t c = 0; c < channels_; ++c) {
      const float x = planar[c][i];
      samplePeak_ = std::max(samplePeak_, std::fabs(x));
      // true peak: interpolate three points between the previous samples
      auto& h = tpHist_[c];
      auto& pos = tpPos_[c];
      h[pos % 12] = x;
      pos = (pos + 1) % 12;
      float tp = std::fabs(x);
      for (std::size_t ph = 0; ph < 3; ++ph) {
        float acc = 0.f;
        for (std::size_t k = 0; k < 12; ++k) acc += phases()[ph][k] * h[(pos + k) % 12];  // oldest first: k=5 is the sample before the new point... see test
        tp = std::max(tp, std::fabs(acc));
      }
      if (tp > 0.f) truePeakDb_ = std::max(truePeakDb_, 20.f * std::log10(tp));
      if (weight_[c] > 0.0) {
        double y = filt_[c][0].run(static_cast<double>(x));
        y = filt_[c][1].run(y);
        sumSq_[c] += y * y;
      }
    }
    if (++stepPos_ >= stepLen_) finishStep();
  }
}

float LoudnessMeter::momentary() const {
  if (stepCount_ < 4) return -200.f;
  double z = 0.0;
  for (std::size_t k = 0; k < 4; ++k) z += steps_[(stepCount_ - 1 - k) % steps_.size()];
  return static_cast<float>(lufsOf(z / 4.0));
}

float LoudnessMeter::shortTerm() const {
  if (stepCount_ < 30) return -200.f;
  double z = 0.0;
  for (double s : steps_) z += s;
  return static_cast<float>(lufsOf(z / 30.0));
}

float LoudnessMeter::integrated() const {
  // Energy of each histogram bin, gated absolutely (already, at -70) and then relatively (-10 LU).
  double sumZ = 0.0; unsigned long total = 0;
  for (std::size_t b = 0; b < kHistBins; ++b) {
    if (!hist_[b]) continue;
    const double l = -70.0 + (static_cast<double>(b) + 0.5) * 0.05;
    sumZ += hist_[b] * std::pow(10.0, (l + 0.691) / 10.0);
    total += hist_[b];
  }
  if (!total) return -200.f;
  const double gate = lufsOf(sumZ / static_cast<double>(total)) - 10.0;
  double z2 = 0.0; unsigned long n2 = 0;
  for (std::size_t b = 0; b < kHistBins; ++b) {
    if (!hist_[b]) continue;
    const double l = -70.0 + (static_cast<double>(b) + 0.5) * 0.05;
    if (l < gate) continue;
    z2 += hist_[b] * std::pow(10.0, (l + 0.691) / 10.0);
    n2 += hist_[b];
  }
  return n2 ? static_cast<float>(lufsOf(z2 / static_cast<double>(n2))) : -200.f;
}

float LoudnessMeter::samplePeakDb() const { return samplePeak_ > 0.f ? 20.f * std::log10(samplePeak_) : -200.f; }

}  // namespace wezt::immersive
