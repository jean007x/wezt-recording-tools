#include "voice/voice_analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

#include "ai/fft.h"
#include "dsp/biquad.h"
#include "music/audio_to_midi.h"

namespace wezt::voice {
namespace {

constexpr double kPi = 3.14159265358979323846;

double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

AudioBuffer slice(const AudioBuffer& a, std::size_t from, std::size_t to) {
  AudioBuffer out(1, to > from ? to - from : 0);
  for (std::size_t i = from; i < to; ++i) out.channel(0)[i - from] = a.channel(0)[i];
  return out;
}

struct Period { double t = 0, len = 0, amp = 0; bool valid = false; };   // start time (s), length (s), peak-to-peak

// Rising zero crossings of the signal low-passed to leave mostly the fundamental. Timing is
// interpolated between samples, so periods are accurate to a small fraction of a sample.
std::vector<Period> findPeriods(const std::vector<float>& x, double sr, double f0) {
  // Zero-phase low-pass (forward then backward), so a crossing of the filtered signal sits exactly where
  // the fundamental crosses zero in the original: no delay, and cycles are not blended with their neighbours.
  std::vector<float> y(x.size());
  const double fc = std::min(1.5 * f0, sr * 0.45);
  const auto lp = dsp::BiquadCoeffs::design(dsp::FilterType::LowPass, sr, fc, 0.0, 0.7071);
  {
    dsp::Biquad b1, b2; b1.c = b2.c = lp;
    for (std::size_t i = 0; i < x.size(); ++i) y[i] = b2.run(b1.run(x[i]));
    dsp::Biquad c1, c2; c1.c = c2.c = lp;
    for (std::size_t i = y.size(); i-- > 0;) y[i] = c2.run(c1.run(y[i]));
  }
  const std::size_t warm = static_cast<std::size_t>(0.04 * sr);
  std::vector<double> cross; std::vector<std::size_t> idx;
  for (std::size_t i = std::max<std::size_t>(1, warm); i < y.size(); ++i)
    if (y[i - 1] < 0.f && y[i] >= 0.f) {
      cross.push_back((static_cast<double>(i - 1) + static_cast<double>(-y[i - 1]) / static_cast<double>(y[i] - y[i - 1])) / sr);
      idx.push_back(i);
    }
  std::vector<Period> out;
  if (cross.size() < 3) return out;
  std::vector<double> lens;
  for (std::size_t k = 0; k + 1 < cross.size(); ++k) lens.push_back(cross[k + 1] - cross[k]);
  const double med = median(lens);
  for (std::size_t k = 0; k + 1 < cross.size(); ++k) {
    Period p; p.t = cross[k]; p.len = lens[k];
    p.valid = p.len > 0.7 * med && p.len < 1.4 * med;
    // Peak-to-peak of the cycle. Known limit: a sharp glottal pulse lands on different sample positions in
    // different cycles, so on a bright, high voice (above about 300 Hz) the shimmer read from a perfectly
    // steady synthetic signal is several percent. Shimmer from bass and tenor-range vowels is reliable.
    float lo = 1e9f, hi = -1e9f;
    for (std::size_t i = idx[k]; i < idx[k + 1] && i < x.size(); ++i) { lo = std::min(lo, x[i]); hi = std::max(hi, x[i]); }
    p.amp = static_cast<double>(hi - lo);
    out.push_back(p);
  }
  return out;
}

// Pitch of each valid period in cents from the median, resampled to `rate` Hz and detrended with a
// moving average (so a slow drift or a change of note is not mistaken for vibrato).
std::vector<double> detrendedCents(const std::vector<Period>& ps, double f0, double rate, double* startTime) {
  std::vector<double> t, c;
  for (auto& p : ps) if (p.valid) { t.push_back(p.t + 0.5 * p.len); c.push_back(1200.0 * std::log2((1.0 / p.len) / f0)); }
  if (t.size() < 8) return {};
  if (startTime) *startTime = t.front();
  const std::size_t n = static_cast<std::size_t>((t.back() - t.front()) * rate);
  std::vector<double> u(n);
  std::size_t j = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double ti = t.front() + static_cast<double>(i) / rate;
    while (j + 2 < t.size() && t[j + 1] < ti) ++j;
    const double w = std::clamp((ti - t[j]) / std::max(1e-9, t[j + 1] - t[j]), 0.0, 1.0);
    u[i] = c[j] * (1 - w) + c[j + 1] * w;
  }
  auto movingAverage = [](const std::vector<double>& v, std::size_t half) {
    std::vector<double> o(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
      const std::size_t a = i > half ? i - half : 0, b = std::min(v.size(), i + half + 1);
      double s = 0; for (std::size_t k = a; k < b; ++k) s += v[k];
      o[i] = s / static_cast<double>(b - a);
    }
    return o;
  };
  const auto trend = movingAverage(u, static_cast<std::size_t>(0.2 * rate));       // 0.4 s window
  for (std::size_t i = 0; i < n; ++i) u[i] -= trend[i];
  return movingAverage(u, static_cast<std::size_t>(0.0125 * rate));                 // ~25 ms: takes the cycle-to-cycle jitter out
}

// Gain of the detrending (0.4 s moving average removed) and smoothing (25 ms moving average) at a
// vibrato rate, so the depth can be corrected for what the two filters take away.
double vibratoFilterGain(double hz) {
  auto sinc = [](double x) { return std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x); };
  return (1.0 - sinc(hz * 0.405)) * sinc(hz * 0.025);
}

// Roots of z^p + a1 z^(p-1) + ... + ap (Durand-Kerner).
std::vector<std::complex<double>> polyRoots(const std::vector<double>& a) {
  const std::size_t p = a.size();
  std::vector<std::complex<double>> r(p);
  for (std::size_t i = 0; i < p; ++i) r[i] = std::polar(0.9, 2.0 * kPi * (static_cast<double>(i) + 0.3) / static_cast<double>(p));
  for (int it = 0; it < 300; ++it) {
    double change = 0;
    for (std::size_t i = 0; i < p; ++i) {
      std::complex<double> v(1, 0);
      for (std::size_t k = 0; k < p; ++k) v = v * r[i] + a[k];
      std::complex<double> den(1, 0);
      for (std::size_t k = 0; k < p; ++k) if (k != i) den *= (r[i] - r[k]);
      if (std::abs(den) < 1e-300) continue;
      const auto d = v / den;
      r[i] -= d;
      change = std::max(change, std::abs(d));
    }
    if (change < 1e-12) break;
  }
  return r;
}

// Formants of one frame by linear prediction (order 12 at about 11 kHz).
std::vector<double> lpcFormants(const std::vector<float>& x, double sr) {
  const std::size_t factor = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(sr / 11025.0)));
  const double sr2 = sr / static_cast<double>(factor);
  std::vector<double> y;
  {
    dsp::Biquad b1, b2;
    b1.c = b2.c = dsp::BiquadCoeffs::design(dsp::FilterType::LowPass, sr, std::min(5000.0, sr2 * 0.45), 0.0, 0.7071);
    std::size_t k = 0;
    for (float s : x) { const float f = b2.run(b1.run(s)); if (k++ % factor == 0) y.push_back(f); }
  }
  if (y.size() < 64) return {};
  for (std::size_t i = y.size(); i-- > 1;) y[i] -= 0.97 * y[i - 1];           // pre-emphasis
  for (std::size_t i = 0; i < y.size(); ++i) y[i] *= 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(y.size() - 1));
  const int order = 12;
  std::vector<double> r(static_cast<std::size_t>(order) + 1, 0.0);
  for (int k = 0; k <= order; ++k) for (std::size_t i = static_cast<std::size_t>(k); i < y.size(); ++i) r[static_cast<std::size_t>(k)] += y[i] * y[i - static_cast<std::size_t>(k)];
  if (r[0] < 1e-12) return {};
  r[0] *= 1.0001;
  std::vector<double> a(static_cast<std::size_t>(order) + 1, 0.0), tmp;
  a[0] = 1; double err = r[0];
  for (int i = 1; i <= order; ++i) {
    double acc = r[static_cast<std::size_t>(i)];
    for (int j = 1; j < i; ++j) acc += a[static_cast<std::size_t>(j)] * r[static_cast<std::size_t>(i - j)];
    const double k = -acc / err;
    tmp = a;
    for (int j = 1; j < i; ++j) a[static_cast<std::size_t>(j)] = tmp[static_cast<std::size_t>(j)] + k * tmp[static_cast<std::size_t>(i - j)];
    a[static_cast<std::size_t>(i)] = k;
    err *= (1 - k * k);
    if (err <= 0) return {};
  }
  const auto roots = polyRoots(std::vector<double>(a.begin() + 1, a.end()));
  std::vector<std::pair<double, double>> found;   // frequency, bandwidth
  for (auto& z : roots) {
    if (z.imag() <= 0.01 || std::abs(z) >= 1.0) continue;
    const double f = std::atan2(z.imag(), z.real()) * sr2 / (2 * kPi), bw = -std::log(std::abs(z)) * sr2 / kPi;
    if (f > 90 && f < sr2 * 0.46 && bw < 400) found.push_back({f, bw});
  }
  std::sort(found.begin(), found.end());
  std::vector<double> out;
  for (auto& [f, bw] : found) { (void)bw; out.push_back(f); }
  return out;
}

}  // namespace

bool findSteadySegment(const AudioBuffer& audio, double sr, double minSeconds, double& fromSec, double& toSec) {
  if (audio.frames == 0 || audio.channels == 0) return false;
  const auto fr = music::trackPitch(audio, sr);
  double bestA = 0, bestB = 0;
  std::size_t i = 0;
  while (i < fr.size()) {
    if (fr[i].hz <= 0 || fr[i].confidence < 0.5) { ++i; continue; }
    std::size_t j = i; std::vector<double> semis;
    while (j < fr.size() && fr[j].hz > 0 && fr[j].confidence >= 0.5) {
      const double s = 12 * std::log2(fr[j].hz / 440.0);
      if (!semis.empty() && std::fabs(s - median(semis)) > 1.0) break;
      semis.push_back(s); ++j;
    }
    if (j > i && fr[j - 1].time - fr[i].time > bestB - bestA) { bestA = fr[i].time; bestB = fr[j - 1].time; }
    i = std::max(j, i + 1);
  }
  if (bestB - bestA < minSeconds) return false;
  fromSec = bestA; toSec = bestB;
  return true;
}

std::vector<double> vibratoEnvelope(const AudioBuffer& audio, double sr, double fromSec, double toSec, double windowSec) {
  std::vector<double> env;
  if (audio.frames == 0) return env;
  const std::size_t a = static_cast<std::size_t>(std::max(0.0, fromSec) * sr), b = std::min(audio.frames, toSec < 0 ? audio.frames : static_cast<std::size_t>(toSec * sr));
  if (b <= a + static_cast<std::size_t>(0.3 * sr)) return env;
  const AudioBuffer seg = slice(audio, a, b);
  const auto fr = music::trackPitch(seg, sr);
  std::vector<double> hz; for (auto& f : fr) if (f.hz > 0 && f.confidence > 0.5) hz.push_back(f.hz);
  const double f0 = median(hz);
  if (f0 <= 0) return env;
  std::vector<float> x(seg.channel(0), seg.channel(0) + seg.frames);
  const auto ps = findPeriods(x, sr, f0);
  double t0 = 0;
  const auto d = detrendedCents(ps, f0, 200.0, &t0);
  const std::size_t w = std::max<std::size_t>(4, static_cast<std::size_t>(windowSec * 200.0));
  for (std::size_t i = 0; i + w <= d.size(); i += w) {
    double s = 0; for (std::size_t k = i; k < i + w; ++k) s += d[k] * d[k];
    env.push_back(2.0 * std::sqrt(2.0) * std::sqrt(s / static_cast<double>(w)) / vibratoFilterGain(5.8));
  }
  return env;
}

VoiceMeasures analyzeVoice(const AudioBuffer& audio, double sr, double fromSec, double toSec) {
  VoiceMeasures m;
  if (audio.frames == 0 || audio.channels == 0 || sr <= 0) { m.message = "no audio"; return m; }
  const std::size_t a = static_cast<std::size_t>(std::max(0.0, fromSec) * sr), b = std::min(audio.frames, toSec < 0 ? audio.frames : static_cast<std::size_t>(toSec * sr));
  if (b <= a + static_cast<std::size_t>(0.25 * sr)) { m.message = "need at least a quarter of a second of one steady vowel"; return m; }
  m.from = static_cast<double>(a) / sr; m.to = static_cast<double>(b) / sr;
  const AudioBuffer seg = slice(audio, a, b);
  const auto fr = music::trackPitch(seg, sr);
  std::vector<double> hz; for (auto& f : fr) if (f.hz > 0 && f.confidence > 0.5) hz.push_back(f.hz);
  if (hz.size() < 5) { m.message = "no steady pitch found in this stretch"; return m; }
  m.f0Hz = median(hz);
  std::vector<float> x(seg.channel(0), seg.channel(0) + seg.frames);
  const auto ps = findPeriods(x, sr, m.f0Hz);
  std::vector<Period> v; for (auto& p : ps) if (p.valid) v.push_back(p);
  m.periods = static_cast<int>(v.size());
  if (v.size() < 12) { m.message = "too few clean pitch periods to measure"; return m; }

  // jitter and shimmer. "local" compares neighbouring cycles; RAP/APQ3 compare each cycle with the
  // mean of three, so slow changes (vibrato, drift) do not count as jitter.
  double meanP = 0, meanA = 0; for (auto& p : v) { meanP += p.len; meanA += p.amp; } meanP /= static_cast<double>(v.size()); meanA /= static_cast<double>(v.size());
  double dl = 0, ds = 0, rap = 0, apq = 0; int nl = 0, nr = 0;
  for (std::size_t k = 0; k + 1 < ps.size(); ++k) if (ps[k].valid && ps[k + 1].valid) { dl += std::fabs(ps[k + 1].len - ps[k].len); ds += std::fabs(ps[k + 1].amp - ps[k].amp); ++nl; }
  for (std::size_t k = 1; k + 1 < ps.size(); ++k) if (ps[k - 1].valid && ps[k].valid && ps[k + 1].valid) {
    rap += std::fabs(ps[k].len - (ps[k - 1].len + ps[k].len + ps[k + 1].len) / 3.0);
    apq += std::fabs(ps[k].amp - (ps[k - 1].amp + ps[k].amp + ps[k + 1].amp) / 3.0); ++nr;
  }
  if (nl) { m.jitterLocalPct = 100.0 * dl / nl / meanP; m.shimmerLocalPct = 100.0 * ds / nl / meanA; }
  if (nr) { m.jitterRapPct = 100.0 * rap / nr / meanP; m.shimmerApq3Pct = 100.0 * apq / nr / meanA; }

  // vibrato: rate from the autocorrelation of the detrended pitch curve, depth from its RMS (a
  // sinusoid of peak-to-peak P has RMS P / (2 sqrt 2)).
  {
    const double rate = 200.0;
    const auto d = detrendedCents(ps, m.f0Hz, rate, nullptr);
    if (d.size() > static_cast<std::size_t>(0.6 * rate)) {
      double e0 = 0; for (double s : d) e0 += s * s;
      double bestR = 0; std::size_t bestLag = 0;
      std::vector<double> ac;
      const std::size_t lo = static_cast<std::size_t>(rate / 9.0), hi = std::min(static_cast<std::size_t>(rate / 3.5), d.size() / 2);
      for (std::size_t lag = lo; lag <= hi + 1 && lag < d.size(); ++lag) {
        double s = 0; for (std::size_t i = 0; i + lag < d.size(); ++i) s += d[i] * d[i + lag];
        ac.push_back(e0 > 0 ? s / e0 : 0);
      }
      for (std::size_t k = 1; k + 1 < ac.size(); ++k) if (ac[k] > bestR && ac[k] >= ac[k - 1] && ac[k] >= ac[k + 1]) { bestR = ac[k]; bestLag = lo + k; }
      m.vibratoStrength = std::max(0.0, bestR);
      if (bestR > 0.3 && bestLag) {
        const double hz = rate / static_cast<double>(bestLag);
        const double ext = 2.0 * std::sqrt(2.0) * std::sqrt(e0 / static_cast<double>(d.size())) / std::max(0.2, vibratoFilterGain(hz));
        if (ext >= 10.0) { m.vibratoRateHz = hz; m.vibratoExtentCents = ext; }   // under 10 cents peak to peak is not a vibrato, just a steady note
      }
    }
  }

  // harmonics-to-noise ratio: normalised autocorrelation at the period, over windows of four periods
  {
    const std::size_t T = static_cast<std::size_t>(sr / m.f0Hz), W = 4 * T;
    double sumR = 0; int n = 0;
    for (std::size_t s0 = 0; s0 + W + 2 * T < x.size(); s0 += T) {
      double best = 0;
      for (std::size_t lag = static_cast<std::size_t>(0.94 * static_cast<double>(T)); lag <= static_cast<std::size_t>(1.06 * static_cast<double>(T)) + 1; ++lag) {
        double c = 0, e1 = 0, e2 = 0;
        for (std::size_t i = s0; i < s0 + W; ++i) { c += x[i] * x[i + lag]; e1 += x[i] * x[i]; e2 += x[i + lag] * x[i + lag]; }
        best = std::max(best, c / std::sqrt(e1 * e2 + 1e-30));
      }
      sumR += best; ++n;
    }
    if (n) { const double r = std::min(0.99999, sumR / n); m.hnrDb = r > 0 ? 10.0 * std::log10(r / (1.0 - r)) : -20.0; }
  }

  // alpha ratio from the average power spectrum (Hann, 4096-point, 50% overlap)
  {
    const std::size_t N = 4096;
    ai::FFT fft(N);
    std::vector<double> pw(N / 2, 0.0); int frames = 0;
    for (std::size_t s0 = 0; s0 + N <= x.size(); s0 += N / 2) {
      std::vector<std::complex<double>> buf(N);
      for (std::size_t i = 0; i < N; ++i) buf[i] = x[s0 + i] * (0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(N - 1)));
      fft.forward(buf);
      for (std::size_t i = 0; i < N / 2; ++i) pw[i] += std::norm(buf[i]);
      ++frames;
    }
    if (frames) {
      auto band = [&](double f0, double f1) { double e = 0; for (std::size_t i = 0; i < N / 2; ++i) { const double f = static_cast<double>(i) * sr / static_cast<double>(N); if (f >= f0 && f < f1) e += pw[i]; } return e; };
      const double lowE = band(50, 1000), highE = band(1000, std::min(5000.0, sr * 0.49));
      if (lowE > 0 && highE > 0) m.alphaRatioDb = 10.0 * std::log10(highE / lowE);
    }
  }

  // formants: median over 15 frames of 50 ms spread over the middle of the segment (three frames were too easily thrown by one bad frame)
  {
    std::vector<std::vector<double>> per(3);
    const std::size_t L = static_cast<std::size_t>(0.05 * sr);
    for (int fi = 0; fi < 15; ++fi) {
      const double where = 0.15 + 0.7 * fi / 14.0;
      const std::size_t c = static_cast<std::size_t>(where * static_cast<double>(x.size()));
      if (c < L / 2 || c + L / 2 > x.size()) continue;
      const auto f = lpcFormants(std::vector<float>(x.begin() + static_cast<std::ptrdiff_t>(c - L / 2), x.begin() + static_cast<std::ptrdiff_t>(c + L / 2)), sr);
      for (std::size_t k = 0; k < 3 && k < f.size(); ++k) per[k].push_back(f[k]);
    }
    for (std::size_t k = 0; k < 3; ++k) m.formantHz[k] = median(per[k]);
  }
  m.ok = true;
  return m;
}

std::string describeMeasures(const VoiceMeasures& m) {
  if (!m.ok) return "Could not measure: " + m.message;
  char b[1400];
  std::snprintf(b, sizeof b,
      "Analysed %.2f-%.2f s, %d cycles, fundamental %.1f Hz.\n"
      "  vibrato      %s (rate %.2f Hz, extent %.0f cents peak-to-peak, regularity %.2f)   [sung vibrato is usually 5-7 Hz and about 50-200 cents peak-to-peak]\n"
      "  jitter       local %.2f %%, RAP %.2f %%   [commonly cited healthy-voice limit for local jitter: about 1.0 %%; trained singers are usually well under it]\n"
      "  shimmer      local %.2f %%, APQ3 %.2f %%   [commonly cited healthy-voice limit for local shimmer: about 3.8 %%]\n"
      "  HNR          %.1f dB   [healthy sustained vowels are usually above about 20 dB]\n"
      "  alpha ratio  %.1f dB (1-5 kHz against 50 Hz-1 kHz)\n"
      "  formants     F1 %.0f Hz, F2 %.0f Hz, F3 %.0f Hz",
      m.from, m.to, m.periods, m.f0Hz, m.vibratoRateHz > 0 ? "found" : "none found", m.vibratoRateHz, m.vibratoExtentCents, m.vibratoStrength, m.jitterLocalPct, m.jitterRapPct,
      m.shimmerLocalPct, m.shimmerApq3Pct, m.hnrDb, m.alphaRatioDb, m.formantHz[0], m.formantHz[1], m.formantHz[2]);
  return b;
}

}  // namespace wezt::voice

namespace wezt::voice {
bool measureFormants(const AudioBuffer& audio, double sampleRate, double& f1, double& f2) {
  const auto m = analyzeVoice(audio, sampleRate, 0.0, -1.0);
  if (!m.ok) return false;
  f1 = m.formantHz[0]; f2 = m.formantHz[1]; return f1 > 0 && f2 > 0;
}
}  // namespace wezt::voice
