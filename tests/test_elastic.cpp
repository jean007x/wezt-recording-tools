#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>

#include "dsp/elastic.h"
#include "music/audio_to_midi.h"

using namespace wezt;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static double medianHz(const AudioBuffer& b, double sr, double t0, double t1) {
  std::vector<double> v;
  for (auto& f : music::trackPitch(b, sr)) if (f.hz > 0 && f.confidence > 0.5 && f.time >= t0 && f.time <= t1) v.push_back(f.hz);
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
static double rms(const AudioBuffer& b, std::size_t a, std::size_t e) {
  double s = 0; for (std::size_t i = a; i < e && i < b.frames; ++i) s += b.channel(0)[i] * b.channel(0)[i];
  return std::sqrt(s / std::max<std::size_t>(1, e - a));
}

int main() {
  const double sr = 22050;
  AudioBuffer tone(1, static_cast<std::size_t>(sr * 2));
  for (std::size_t i = 0; i < tone.frames; ++i) {
    const double t = i / sr;
    tone.channel(0)[i] = static_cast<float>(0.4 * std::sin(2 * M_PI * 220 * t) + 0.2 * std::sin(2 * M_PI * 440 * t) + 0.1 * std::sin(2 * M_PI * 660 * t));
  }
  for (auto mode : {dsp::ElasticMode::Polyphonic, dsp::ElasticMode::Rhythmic, dsp::ElasticMode::Monophonic}) {
    for (double ratio : {0.75, 1.5}) {
      const auto y = dsp::elasticStretch(tone, ratio, mode, sr);
      CHECK(std::llabs(static_cast<long long>(y.frames) - static_cast<long long>(tone.frames * ratio)) <= 1);
      const double hz = medianHz(y, sr, 0.5, 1.2);
      if (std::fabs(hz - 220) > 6) std::printf("mode %s ratio %.2f pitch %.1f\n", dsp::elasticModeName(mode), ratio, hz);
      CHECK(std::fabs(hz - 220) < 6);                              // pitch unchanged
      const double a = rms(tone, 10000, 30000), b = rms(y, 10000, 30000);
      CHECK(b > a * 0.6 && b < a * 1.5);                            // level kept
    }
  }
  {  // pitch shift keeps the length
    const auto y = dsp::elasticPitch(tone, 5.0, dsp::ElasticMode::Polyphonic, sr);
    CHECK(y.frames == tone.frames);
    const double hz = medianHz(y, sr, 0.5, 1.2);
    if (std::fabs(hz - 220 * std::pow(2.0, 5 / 12.0)) > 8) std::printf("pitch poly %.1f\n", hz);
    CHECK(std::fabs(hz - 220 * std::pow(2.0, 5 / 12.0)) < 8);
  }
  {  // varispeed changes both
    const auto y = dsp::elasticStretch(tone, 2.0, dsp::ElasticMode::Varispeed, sr);
    CHECK(std::fabs(medianHz(y, sr, 0.5, 1.5) - 110) < 4);
  }
  {  // rhythmic mode keeps hits sharp: a click train stretched keeps a high crest factor
    AudioBuffer clicks(1, static_cast<std::size_t>(sr * 2));
    for (std::size_t i = 0; i < clicks.frames; i += static_cast<std::size_t>(sr / 4)) for (int k = 0; k < 200; ++k) if (i + k < clicks.frames) clicks.channel(0)[i + k] = static_cast<float>(0.9 * std::exp(-k / 40.0) * std::sin(k * 0.8));
    auto crest = [&](const AudioBuffer& b) { return b.peak() / static_cast<float>(std::max(1e-6, rms(b, 0, b.frames))); };
    const auto r = dsp::elasticStretch(clicks, 1.4, dsp::ElasticMode::Rhythmic, sr);
    const auto p = dsp::elasticStretch(clicks, 1.4, dsp::ElasticMode::Polyphonic, sr);
    std::printf("crest in %.1f rhythmic %.1f poly %.1f\n", crest(clicks), crest(r), crest(p));
    CHECK(crest(r) >= crest(p) * 0.95);
  }
  CHECK(dsp::elasticStretch(AudioBuffer(), 1.5, dsp::ElasticMode::Polyphonic, sr).frames == 0);
  CHECK(dsp::elasticStretch(tone, 0.0, dsp::ElasticMode::Polyphonic, sr).frames == 0);
  std::printf(failures ? "FAILED\n" : "elastic tests passed\n");
  return failures;
}
