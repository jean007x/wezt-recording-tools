#include "music/psola.h"

#include <algorithm>
#include <cmath>

#include "music/audio_to_midi.h"

namespace wezt::music {
namespace {
struct Mark { std::size_t pos; std::size_t period; bool voiced; };

std::vector<Mark> findMarks(const std::vector<float>& x, double sr) {
  AudioBuffer b(1, x.size());
  std::copy(x.begin(), x.end(), b.channel(0));
  const auto f0 = trackPitch(b, sr, 70, 1000);
  auto hzAt = [&](double t) {   // nearest frame; 0 when unvoiced
    if (f0.empty()) return 0.0;
    std::size_t best = 0; double bd = 1e9;
    for (std::size_t i = 0; i < f0.size(); ++i) { const double d = std::fabs(f0[i].time - t); if (d < bd) { bd = d; best = i; } }
    return f0[best].confidence > 0.45 ? f0[best].hz : 0.0;
  };
  const std::size_t unvoicedPeriod = std::max<std::size_t>(32, static_cast<std::size_t>(0.008 * sr));
  std::vector<Mark> m;
  std::size_t pos = 0;
  while (pos < x.size()) {
    const double hz = hzAt(static_cast<double>(pos) / sr);
    const bool voiced = hz > 0;
    const std::size_t period = voiced ? static_cast<std::size_t>(sr / hz) : unvoicedPeriod;
    std::size_t at = pos;
    if (voiced) {   // snap to the strongest sample near the predicted position
      const std::size_t lo = pos > period / 4 ? pos - period / 4 : 0, hi = std::min(x.size() - 1, pos + period / 4);
      float best = -1.f;
      for (std::size_t i = lo; i <= hi; ++i) if (std::fabs(x[i]) > best) { best = std::fabs(x[i]); at = i; }
      if (!m.empty() && at <= m.back().pos) at = m.back().pos + 1;
    }
    m.push_back({at, period, voiced});
    pos = at + period;
  }
  return m;
}
}  // namespace

std::vector<float> psola(const std::vector<float>& x, double sr, double pitchRatio, std::size_t outLen) {
  std::vector<float> y(outLen, 0.f), norm(outLen, 0.f);
  if (x.size() < 64 || outLen == 0 || pitchRatio <= 0) return y;
  const auto marks = findMarks(x, sr);
  if (marks.empty()) return y;
  const double timeMap = static_cast<double>(x.size()) / static_cast<double>(outLen);
  double s = 0;
  std::size_t k = 0;
  while (s < static_cast<double>(outLen)) {
    const double a = s * timeMap;
    while (k + 1 < marks.size() && std::fabs(static_cast<double>(marks[k + 1].pos) - a) < std::fabs(static_cast<double>(marks[k].pos) - a)) ++k;
    const Mark& mk = marks[k];
    const std::size_t half = std::max<std::size_t>(8, mk.period);   // grain = two periods
    const long centre = static_cast<long>(mk.pos);
    for (long i = -static_cast<long>(half); i < static_cast<long>(half); ++i) {
      const long src = centre + i, dst = static_cast<long>(s) + i;
      if (src < 0 || src >= static_cast<long>(x.size()) || dst < 0 || dst >= static_cast<long>(outLen)) continue;
      const float w = 0.5f + 0.5f * std::cos(3.14159265f * static_cast<float>(i) / static_cast<float>(half));
      y[static_cast<std::size_t>(dst)] += x[static_cast<std::size_t>(src)] * w;
      norm[static_cast<std::size_t>(dst)] += w;
    }
    s += static_cast<double>(mk.period) / (mk.voiced ? pitchRatio : 1.0);
  }
  for (std::size_t i = 0; i < outLen; ++i) if (norm[i] > 0.2f) y[i] /= norm[i]; else y[i] = 0.f;
  return y;
}

}  // namespace wezt::music
