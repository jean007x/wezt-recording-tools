// Calibrates the voice measurements on signals whose vibrato, jitter, shimmer, noise and formants are
// known exactly, so the numbers can be trusted when they are used to judge the singing synthesizer
// (and recordings of real voices).
#include <cmath>
#include <cstdio>
#include <vector>

#include "music/theory.h"
#include "voice/voice_analysis.h"

using namespace wezt;
using namespace wezt::voice;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static double gauss(music::Rng& r) { return std::sqrt(-2.0 * std::log(std::max(1e-12, r.uniform()))) * std::cos(2 * M_PI * r.uniform()); }

struct Spec { double f0 = 200, vibHz = 0, vibCents = 0, jitter = 0, shimmer = 0, snrDb = 200, seconds = 2.0, sr = 48000; };

// 20 harmonics (1/k) with a known vibrato (peak `vibCents`), per-cycle Gaussian jitter and shimmer (relative sigma) and white noise at `snrDb`.
static AudioBuffer make(const Spec& s, std::uint64_t seed = 7) {
  music::Rng rng(seed);
  const std::size_t n = static_cast<std::size_t>(s.seconds * s.sr);
  AudioBuffer a(1, n);
  double phase = 0, jit = 0, amp = 1, power = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / s.sr;
    const double f = s.f0 * std::pow(2.0, s.vibCents * std::sin(2 * M_PI * s.vibHz * t) / 1200.0) * (1.0 + jit);
    phase += f / s.sr;
    if (phase >= 1.0) { phase -= 1.0; jit = s.jitter * gauss(rng); amp = 1.0 + s.shimmer * gauss(rng); }
    double v = 0;
    for (int k = 1; k <= 20; ++k) v += std::sin(2 * M_PI * k * phase) / k;
    a.channel(0)[i] = static_cast<float>(amp * v);
    power += v * v;
  }
  power /= static_cast<double>(n);
  if (s.snrDb < 100) {
    const double sigma = std::sqrt(power / std::pow(10.0, s.snrDb / 10.0));
    for (std::size_t i = 0; i < n; ++i) a.channel(0)[i] += static_cast<float>(sigma * gauss(rng));
  }
  return a;
}

int main() {
  {  // a clean, steady tone: no vibrato, no jitter
    Spec s; auto m = analyzeVoice(make(s), s.sr);
    CHECK(m.ok);
    std::printf("steady: f0 %.2f, jitter %.3f%%, shimmer %.3f%%, vibrato %.2f Hz strength %.2f, HNR %.1f\n", m.f0Hz, m.jitterLocalPct, m.shimmerLocalPct, m.vibratoRateHz, m.vibratoStrength, m.hnrDb);
    CHECK(std::fabs(m.f0Hz - 200) < 1.0);
    CHECK(m.jitterLocalPct < 0.03 && m.shimmerLocalPct < 0.3);
    CHECK(m.vibratoRateHz == 0 || m.vibratoExtentCents < 3.0);
    CHECK(m.hnrDb > 35);
  }
  {  // known vibrato: 6 Hz, +-25 cents = 50 cents peak to peak
    Spec s; s.vibHz = 6.0; s.vibCents = 25; auto m = analyzeVoice(make(s), s.sr);
    std::printf("vibrato: rate %.3f Hz, extent %.1f cents (true 6, 50), strength %.2f\n", m.vibratoRateHz, m.vibratoExtentCents, m.vibratoStrength);
    CHECK(m.ok && std::fabs(m.vibratoRateHz - 6.0) < 0.3);
    CHECK(std::fabs(m.vibratoExtentCents - 50.0) < 5.0);
    CHECK(m.jitterRapPct < 0.08);   // vibrato must not be mistaken for jitter by the RAP measure
  }
  {  // known jitter: sigma 0.4 % of the period. Mean |difference| of neighbouring cycles = 2 sigma / sqrt(pi) = 1.128 sigma.
    // (Jitter and shimmer are measured separately here: on a sawtooth-like waveform a sudden amplitude change at the
    // cycle boundary moves the filtered zero crossing, so one contaminates the other. That is true of any
    // period-based jitter tool, not only this one.)
    Spec s; s.jitter = 0.004; s.seconds = 4; auto m = analyzeVoice(make(s), s.sr);
    std::printf("jitter local %.3f%% (expect ~0.45), RAP %.3f%% (~0.26)\n", m.jitterLocalPct, m.jitterRapPct);
    CHECK(std::fabs(m.jitterLocalPct - 0.45) < 0.1);
    CHECK(std::fabs(m.jitterRapPct - 0.26) < 0.07);
    CHECK(m.shimmerLocalPct < 0.4);                          // no amplitude variation was put in
  }
  {  // known shimmer: sigma 2 % of the amplitude, no jitter
    Spec s; s.shimmer = 0.02; s.seconds = 4; auto m = analyzeVoice(make(s), s.sr);
    std::printf("shimmer local %.2f%% (expect ~2.26), APQ3 %.2f%% (~1.3)\n", m.shimmerLocalPct, m.shimmerApq3Pct);
    CHECK(std::fabs(m.shimmerLocalPct - 2.26) < 0.4);
    CHECK(std::fabs(m.shimmerApq3Pct - 1.3) < 0.3);
  }
  {  // known noise: 20 dB harmonics-to-noise
    Spec s; s.snrDb = 20; auto m = analyzeVoice(make(s), s.sr);
    std::printf("HNR %.1f dB (true 20)\n", m.hnrDb);
    CHECK(std::fabs(m.hnrDb - 20.0) < 3.0);
    Spec s2; s2.snrDb = 10; auto m2 = analyzeVoice(make(s2), s2.sr);
    CHECK(m2.hnrDb < m.hnrDb - 6.0);
  }
  {  // vibrato that starts late: the envelope grows
    Spec s; s.seconds = 2.0; auto a = make(s);
    AudioBuffer b(1, a.frames);
    music::Rng rng(3);
    double phase = 0;
    for (std::size_t i = 0; i < a.frames; ++i) {
      const double t = static_cast<double>(i) / s.sr;
      const double depth = t < 0.6 ? 0.0 : std::min(1.0, (t - 0.6) / 0.4) * 40.0;
      phase += 200.0 * std::pow(2.0, depth * std::sin(2 * M_PI * 5.8 * t) / 1200.0) / s.sr; if (phase >= 1) phase -= 1;
      double v = 0; for (int k = 1; k <= 20; ++k) v += std::sin(2 * M_PI * k * phase) / k;
      b.channel(0)[i] = static_cast<float>(v);
    }
    const auto env = vibratoEnvelope(b, s.sr, 0.0, -1, 0.3);
    std::printf("vibrato envelope (cents p-p per 0.3 s):"); for (double e : env) std::printf(" %.0f", e); std::printf("\n");
    CHECK(env.size() >= 4 && env.front() < 8.0 && env.back() > 40.0);
  }
  {  // formants: impulse train through resonators at 730 / 1090 / 2440 Hz (the vowel "ah" of an adult male)
    const double sr = 22050; const std::size_t n = static_cast<std::size_t>(sr);
    AudioBuffer a(1, n);
    struct R { double a1, a2, y1 = 0, y2 = 0; } r[3];
    const double F[3] = {730, 1090, 2440}, B[3] = {80, 90, 120};
    for (int i = 0; i < 3; ++i) { const double rr = std::exp(-M_PI * B[i] / sr); r[i].a1 = 2 * rr * std::cos(2 * M_PI * F[i] / sr); r[i].a2 = -rr * rr; }
    for (std::size_t i = 0; i < n; ++i) {
      double x = (i % static_cast<std::size_t>(sr / 120.0) == 0) ? 1.0 : 0.0;
      for (auto& q : r) { const double y = x + q.a1 * q.y1 + q.a2 * q.y2; q.y2 = q.y1; q.y1 = y; x = y; }
      a.channel(0)[i] = static_cast<float>(x * 0.01);
    }
    auto m = analyzeVoice(a, sr, 0.2, 0.9);
    std::printf("formants %.0f %.0f %.0f (true 730 1090 2440)\n", m.formantHz[0], m.formantHz[1], m.formantHz[2]);
    CHECK(m.ok && std::fabs(m.formantHz[0] - 730) < 60 && std::fabs(m.formantHz[1] - 1090) < 90 && std::fabs(m.formantHz[2] - 2440) < 200);
  }
  {  // a bad input is refused with a reason, never a crash
    AudioBuffer empty(1, 100);
    CHECK(!analyzeVoice(empty, 48000).ok && !analyzeVoice(AudioBuffer(), 48000).ok);
    AudioBuffer silent(1, 48000);
    CHECK(!analyzeVoice(silent, 48000).ok);
  }
  std::printf(failures ? "FAILED\n" : "voice analysis tests passed\n");
  return failures ? 1 : 0;
}
