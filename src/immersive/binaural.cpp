#include "immersive/binaural.h"

#include <algorithm>
#include <cmath>

namespace wezt::immersive {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kHeadRadius = 0.0875, kSpeedOfSound = 343.0;
}  // namespace

BinauralMonitor::BinauralMonitor(const SpeakerLayout& layout) : layout_(layout), paths_(layout.size()) {}

void BinauralMonitor::prepare(double sampleRate, std::size_t) {
  rate_ = sampleRate;
  for (std::size_t i = 0; i < paths_.size(); ++i) {
    Path& p = paths_[i];
    const Speaker& s = layout_.speakers[i];
    p.lfe = s.lfe;
    p.center = std::fabs(std::sin(s.azimuth * kPi / 180.0) * std::cos(s.elevation * kPi / 180.0)) < 1e-6;
    const double az = s.azimuth * kPi / 180.0, el = s.elevation * kPi / 180.0;
    // lateral angle: 0 at the median plane, +90 hard left
    const double sinLat = std::clamp(std::sin(az) * std::cos(el), -1.0, 1.0);
    const double lat = std::asin(sinLat);
    p.farIsLeft = sinLat < 0.0;
    const double itd = (kHeadRadius / kSpeedOfSound) * (std::fabs(lat) + std::fabs(sinLat));  // Woodworth
    p.delay = static_cast<float>(itd * rate_);
    const double side = std::fabs(sinLat);
    p.farGain = static_cast<float>(std::pow(10.0, -6.0 * side / 20.0));
    const double fc = 20000.0 * std::pow(0.08, side);  // 20 kHz at the front down to 1.6 kHz at 90 degrees
    p.lpCoef = static_cast<float>(1.0 - std::exp(-2.0 * kPi * std::min(fc, 0.45 * rate_) / rate_));
    p.ring.assign(std::max<std::size_t>(64, static_cast<std::size_t>(p.delay) + 4), 0.f);
    p.pos = 0;
    p.lpState = 0.f;
  }
}

void BinauralMonitor::process(const AudioBuffer& in, AudioBuffer& out) {
  const std::size_t n = out.frames;
  out.clearValid();
  if (out.channels < 2) return;
  float* L = out.channel(0);
  float* R = out.channel(1);
  for (std::size_t s = 0; s < paths_.size() && s < in.channels; ++s) {
    Path& p = paths_[s];
    const float* x = in.channel(s);
    if (p.lfe) {  // no localisation for the sub channel
      for (std::size_t i = 0; i < n; ++i) { L[i] += 0.7071f * x[i]; R[i] += 0.7071f * x[i]; }
      continue;
    }
    if (p.center) {
      for (std::size_t i = 0; i < n; ++i) { L[i] += 0.7071f * x[i]; R[i] += 0.7071f * x[i]; }
      continue;
    }
    float* near = p.farIsLeft ? R : L;
    float* far = p.farIsLeft ? L : R;
    const std::size_t sz = p.ring.size();
    const float d = p.delay;
    const std::size_t di = static_cast<std::size_t>(d);
    const float frac = d - static_cast<float>(di);
    for (std::size_t i = 0; i < n; ++i) {
      near[i] += x[i];
      p.ring[p.pos] = x[i];
      const float a = p.ring[(p.pos + sz - di) % sz];
      const float b = p.ring[(p.pos + sz - di - 1) % sz];
      const float delayed = a * (1.f - frac) + b * frac;
      p.lpState += p.lpCoef * (delayed - p.lpState);
      far[i] += p.lpState * p.farGain;
      p.pos = (p.pos + 1) % sz;
    }
  }
}

}  // namespace wezt::immersive
