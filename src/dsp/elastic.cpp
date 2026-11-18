#include "dsp/elastic.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <string>

#include "ai/fft.h"
#include "music/psola.h"

namespace wezt::dsp {
namespace {
using Cx = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

double wrap(double a) { return a - 2.0 * kPi * std::round(a / (2.0 * kPi)); }

std::vector<float> resampleLinear(const std::vector<float>& x, double readStep, std::size_t outLen) {
  std::vector<float> y(outLen, 0.f);
  for (std::size_t i = 0; i < outLen; ++i) {
    const double p = static_cast<double>(i) * readStep;
    const std::size_t a = static_cast<std::size_t>(p);
    if (a + 1 >= x.size()) { if (a < x.size()) y[i] = x[a]; continue; }
    const float f = static_cast<float>(p - static_cast<double>(a));
    y[i] = x[a] * (1.f - f) + x[a + 1] * f;
  }
  return y;
}

std::vector<float> phaseVocoder(const std::vector<float>& x, double ratio, bool phaseReset, double sr) {
  const std::size_t N = sr > 60000 ? 4096 : 2048, Hs = N / 4, half = N / 2;
  const std::size_t outLen = static_cast<std::size_t>(std::llround(static_cast<double>(x.size()) * ratio));
  std::vector<float> y(outLen + N, 0.f);
  if (x.size() < 16 || outLen == 0) return std::vector<float>(outLen, 0.f);
  ai::FFT fft(N);
  std::vector<double> w(N);
  for (std::size_t i = 0; i < N; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(N));
  const double norm = 1.5;   // sum of w^2 at 75% overlap
  std::vector<Cx> spec(N);
  std::vector<double> prevPhase(half + 1, 0.0), synPhase(half + 1, 0.0), prevMag(half + 1, 0.0), mag(half + 1), phase(half + 1);
  std::vector<std::size_t> peakOf(half + 1);
  const std::size_t frames = outLen / Hs + 3;
  double prevA = 0;
  for (std::size_t t = 0; t < frames; ++t) {
    const double aD = static_cast<double>(t * Hs) / ratio;
    const long a = static_cast<long>(std::llround(aD));
    const double Ha = t == 0 ? static_cast<double>(Hs) : static_cast<double>(a) - prevA;
    for (std::size_t i = 0; i < N; ++i) {
      const long idx = a - static_cast<long>(half) + static_cast<long>(i);
      spec[i] = Cx(idx >= 0 && idx < static_cast<long>(x.size()) ? static_cast<double>(x[static_cast<std::size_t>(idx)]) * w[i] : 0.0, 0.0);
    }
    fft.forward(spec);
    double flux = 0, total = 1e-9;
    for (std::size_t k = 0; k <= half; ++k) {
      mag[k] = std::abs(spec[k]); phase[k] = std::arg(spec[k]);
      if (k > half / 8) { flux += std::max(0.0, mag[k] - prevMag[k]); total += mag[k]; }
    }
    const bool onset = phaseReset && t > 0 && flux / total > 0.55 && ratio != 1.0;
    if (t == 0 || onset) {
      synPhase = phase;
    } else {
      // spectral peaks, and the peak that rules each bin
      std::vector<std::size_t> peaks;
      for (std::size_t k = 1; k < half; ++k) if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > 1e-6) peaks.push_back(k);
      if (peaks.empty()) peaks.push_back(1);
      std::size_t pi = 0;
      for (std::size_t k = 0; k <= half; ++k) {
        while (pi + 1 < peaks.size() && std::abs(static_cast<double>(peaks[pi + 1]) - static_cast<double>(k)) < std::abs(static_cast<double>(peaks[pi]) - static_cast<double>(k))) ++pi;
        peakOf[k] = peaks[pi];
      }
      std::vector<double> newSyn(half + 1);
      for (std::size_t pk : peaks) {
        const double omega = 2.0 * kPi * static_cast<double>(pk) / static_cast<double>(N);
        const double dphi = wrap(phase[pk] - prevPhase[pk] - Ha * omega);
        newSyn[pk] = synPhase[pk] + static_cast<double>(Hs) * (omega + dphi / std::max(1.0, Ha));
      }
      for (std::size_t k = 0; k <= half; ++k) if (peakOf[k] != k || std::find(peaks.begin(), peaks.end(), k) == peaks.end()) newSyn[k] = newSyn[peakOf[k]] + (phase[k] - phase[peakOf[k]]);
      synPhase = newSyn;
    }
    prevPhase = phase; prevMag = mag; prevA = static_cast<double>(a);
    for (std::size_t k = 0; k <= half; ++k) spec[k] = std::polar(mag[k], synPhase[k]);
    for (std::size_t k = 1; k < half; ++k) spec[N - k] = std::conj(spec[k]);
    spec[half] = Cx(spec[half].real(), 0.0);
    // inverse FFT through the forward one: ifft(x) = conj(fft(conj(x))) / N
    for (auto& c : spec) c = std::conj(c);
    fft.forward(spec);
    const long start = static_cast<long>(t * Hs) - static_cast<long>(half);
    for (std::size_t i = 0; i < N; ++i) {
      const long o = start + static_cast<long>(i);
      if (o < 0 || o >= static_cast<long>(y.size())) continue;
      y[static_cast<std::size_t>(o)] += static_cast<float>(std::conj(spec[i]).real() / static_cast<double>(N) * w[i] / norm);
    }
  }
  y.resize(outLen);
  return y;
}

std::vector<float> channelOf(const AudioBuffer& b, std::size_t c) { return std::vector<float>(b.channel(c), b.channel(c) + b.frames); }

}  // namespace

const char* elasticModeName(ElasticMode m) {
  switch (m) { case ElasticMode::Polyphonic: return "polyphonic"; case ElasticMode::Rhythmic: return "rhythmic"; case ElasticMode::Monophonic: return "monophonic"; default: return "varispeed"; }
}
bool parseElasticMode(const char* t, ElasticMode& m) {
  const std::string s = t ? t : "";
  if (s.rfind("poly", 0) == 0) m = ElasticMode::Polyphonic;
  else if (s.rfind("rhythm", 0) == 0) m = ElasticMode::Rhythmic;
  else if (s.rfind("mono", 0) == 0) m = ElasticMode::Monophonic;
  else if (s.rfind("vari", 0) == 0) m = ElasticMode::Varispeed;
  else return false;
  return true;
}

AudioBuffer elasticStretch(const AudioBuffer& in, double ratio, ElasticMode mode, double sr) {
  if (in.frames == 0 || in.channels == 0 || ratio <= 0.05 || ratio > 20.0) return {};
  const std::size_t outLen = static_cast<std::size_t>(std::llround(static_cast<double>(in.frames) * ratio));
  AudioBuffer out(in.channels, outLen);
  for (std::size_t c = 0; c < in.channels; ++c) {
    const auto x = channelOf(in, c);
    std::vector<float> y;
    switch (mode) {
      case ElasticMode::Polyphonic: y = phaseVocoder(x, ratio, false, sr); break;
      case ElasticMode::Rhythmic: y = phaseVocoder(x, ratio, true, sr); break;
      case ElasticMode::Monophonic: y = music::psola(x, sr, 1.0, outLen); break;
      case ElasticMode::Varispeed: y = resampleLinear(x, 1.0 / ratio, outLen); break;
    }
    y.resize(outLen, 0.f);
    std::copy(y.begin(), y.end(), out.channel(c));
  }
  return out;
}

AudioBuffer elasticPitch(const AudioBuffer& in, double semitones, ElasticMode mode, double sr) {
  if (in.frames == 0 || in.channels == 0) return {};
  const double r = std::pow(2.0, semitones / 12.0);
  if (mode == ElasticMode::Varispeed) {   // length changes too: that is what varispeed means
    AudioBuffer out(in.channels, static_cast<std::size_t>(std::llround(static_cast<double>(in.frames) / r)));
    for (std::size_t c = 0; c < in.channels; ++c) { auto y = resampleLinear(channelOf(in, c), r, out.frames); std::copy(y.begin(), y.end(), out.channel(c)); }
    return out;
  }
  AudioBuffer out(in.channels, in.frames);
  for (std::size_t c = 0; c < in.channels; ++c) {
    const auto x = channelOf(in, c);
    std::vector<float> y;
    if (mode == ElasticMode::Monophonic) y = music::psola(x, sr, r, x.size());
    else {
      // stretch to r times the length (same pitch), then play it back r times faster: pitch x r, length unchanged
      const auto st = phaseVocoder(x, r, mode == ElasticMode::Rhythmic, sr);
      y = resampleLinear(st, r, x.size());
    }
    y.resize(in.frames, 0.f);
    std::copy(y.begin(), y.end(), out.channel(c));
  }
  return out;
}

}  // namespace wezt::dsp
