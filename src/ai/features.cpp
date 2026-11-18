#include "ai/features.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "ai/fft.h"
#include "immersive/layout.h"
#include "immersive/loudness.h"

namespace wezt::ai {

const std::array<float, kBands> kBandCenters = {31.5f, 63.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f};
const char* const kBandNames[kBands] = {"31 Hz", "63 Hz", "125 Hz", "250 Hz", "500 Hz", "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz"};

namespace {
constexpr double kPi = 3.14159265358979323846;

float db10(double p) { return p > 1e-20 ? static_cast<float>(10.0 * std::log10(p)) : -200.f; }

std::vector<double> hann(std::size_t n) {
  std::vector<double> w(n);
  for (std::size_t i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
  return w;
}

// Krumhansl-Kessler key profiles (published perceptual data).
const double kMajor[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
const double kMinor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

double correlate(const double* a, const double* b, int n) {
  double ma = 0, mb = 0;
  for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
  ma /= n; mb /= n;
  double num = 0, da = 0, db = 0;
  for (int i = 0; i < n; ++i) { num += (a[i] - ma) * (b[i] - mb); da += (a[i] - ma) * (a[i] - ma); db += (b[i] - mb) * (b[i] - mb); }
  return da > 0 && db > 0 ? num / std::sqrt(da * db) : 0.0;
}
}  // namespace

std::string keyName(int root, bool minor) {
  static const char* n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (root < 0 || root > 11) return "?";
  return std::string(n[root]) + (minor ? " minor" : " major");
}

Features analyze(const AudioBuffer& a, double sr) {
  Features f;
  if (a.frames < 64 || a.channels == 0 || sr < 8000) return f;
  const std::size_t N = a.frames;
  const float* L = a.channel(0);
  const float* R = a.channels > 1 ? a.channel(1) : a.channel(0);
  f.seconds = static_cast<double>(N) / sr;

  // ---- loudness, true peak, short-term range ----
  {
    immersive::LoudnessMeter lm(sr, immersive::makeLayout("2.0"));
    std::vector<float> lt;
    const std::size_t chunk = static_cast<std::size_t>(sr * 0.5);
    for (std::size_t pos = 0; pos < N; pos += chunk) {
      const std::size_t n = std::min(chunk, N - pos);
      const float* p[2] = {L + pos, R + pos};
      lm.feed(p, n);
      const float st = lm.shortTerm();
      if (st > -70.f && pos >= static_cast<std::size_t>(sr * 3)) lt.push_back(st);
    }
    f.lufs = lm.integrated();
    f.truePeakDb = lm.truePeakDb();
    if (lt.size() >= 4) {
      std::sort(lt.begin(), lt.end());
      f.loudnessRangeLu = lt[static_cast<std::size_t>(0.95 * static_cast<double>(lt.size() - 1))] - lt[static_cast<std::size_t>(0.10 * static_cast<double>(lt.size() - 1))];
    }
  }

  // ---- time-domain: rms, crest, stereo image ----
  double sumM = 0, sumS = 0, sumLR = 0, sumLL = 0, sumRR = 0;
  float peak = 0.f;
  for (std::size_t i = 0; i < N; ++i) {
    const double m = 0.5 * (L[i] + R[i]), s = 0.5 * (L[i] - R[i]);
    sumM += m * m; sumS += s * s;
    sumLR += static_cast<double>(L[i]) * R[i]; sumLL += static_cast<double>(L[i]) * L[i]; sumRR += static_cast<double>(R[i]) * R[i];
    peak = std::max({peak, std::fabs(L[i]), std::fabs(R[i])});
  }
  f.rmsDb = db10(sumM / static_cast<double>(N));
  f.crestDb = peak > 0.f ? 20.f * std::log10(peak) - f.rmsDb : 0.f;
  f.stereoWidth = sumM + sumS > 1e-20 ? static_cast<float>(sumS / (sumM + sumS)) : 0.f;
  f.correlation = sumLL > 0 && sumRR > 0 ? static_cast<float>(sumLR / std::sqrt(sumLL * sumRR)) : 1.f;

  // ---- spectrum, centroid, chroma (one pass over 4096-point frames of the mid signal) ----
  constexpr std::size_t kN = 4096, kHop = 2048;
  FFT fft(kN);
  const auto win = hann(kN);
  std::vector<std::complex<double>> buf(kN);
  std::array<double, kBands> bandP{};
  double total = 0, cenNum = 0, cenDen = 0;
  double chroma[12] = {};
  const double binHz = sr / static_cast<double>(kN);
  const std::size_t maxFrames = 6000;  // bound the work on very long files by striding
  const std::size_t totalFrames = N >= kN ? (N - kN) / kHop + 1 : 0;
  const std::size_t stride = std::max<std::size_t>(1, totalFrames / maxFrames);
  for (std::size_t fr = 0; fr < totalFrames; fr += stride) {
    const std::size_t o = fr * kHop;
    for (std::size_t i = 0; i < kN; ++i) buf[i] = 0.5 * (L[o + i] + R[o + i]) * win[i];
    fft.forward(buf);
    for (std::size_t k = 1; k < kN / 2; ++k) {
      const double hz = static_cast<double>(k) * binHz;
      const double p = std::norm(buf[k]);
      total += p; cenNum += p * hz; cenDen += p;
      if (hz >= 22.0) {
        // octave band index: band b spans [c/sqrt2, c*sqrt2)
        const int b = static_cast<int>(std::floor(std::log2(hz / 31.5) + 0.5));
        if (b >= 0 && b < kBands) bandP[static_cast<std::size_t>(b)] += p;
      }
    }
  }
  if (total > 0) {
    for (int b = 0; b < kBands; ++b) f.bandDb[static_cast<std::size_t>(b)] = db10(bandP[static_cast<std::size_t>(b)] / total);
    f.centroidHz = static_cast<float>(cenNum / cenDen);
  }
  // key: a longer FFT, because at 4096 points one bin is almost a semitone wide at 260 Hz
  {
    constexpr std::size_t cN = 16384;
    if (N >= cN) {
      FFT cf(cN);
      const auto cw = hann(cN);
      std::vector<std::complex<double>> cb(cN);
      const std::size_t cFrames = (N - cN) / cN + 1, cStride = std::max<std::size_t>(1, cFrames / 400);
      const double cBin = sr / static_cast<double>(cN);
      for (std::size_t fr = 0; fr < cFrames; fr += cStride) {
        for (std::size_t i = 0; i < cN; ++i) cb[i] = 0.5 * (L[fr * cN + i] + R[fr * cN + i]) * cw[i];
        cf.forward(cb);
        for (std::size_t k = 1; k < cN / 2; ++k) {
          const double hz = static_cast<double>(k) * cBin;
          if (hz < 100.0 || hz > 4000.0) continue;
          const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
          const int pc = ((static_cast<int>(std::lround(midi)) % 12) + 12) % 12;
          chroma[pc] += std::abs(cb[k]);
        }
      }
    }
  }
  {
    double bestR = -2, secondR = -2;
    int bestRoot = -1; bool bestMinor = false;
    double sum = 0; for (double c : chroma) sum += c;
    if (sum > 0) {
      for (int minor = 0; minor < 2; ++minor)
        for (int r = 0; r < 12; ++r) {
          double prof[12];
          const double* base = minor ? kMinor : kMajor;  // a pointer, not a conditional array lvalue (GCC's bounds check mishandles that)
          for (int i = 0; i < 12; ++i) prof[i] = base[(i - r + 12) % 12];
          const double c = correlate(chroma, prof, 12);
          if (c > bestR) { secondR = bestR; bestR = c; bestRoot = r; bestMinor = minor; }
          else if (c > secondR) secondR = c;
        }
      f.keyRoot = bestRoot; f.minor = bestMinor;
      f.keyConfidence = static_cast<float>(std::clamp(bestR - secondR, 0.0, 1.0));
    }
  }

  // ---- tempo: spectral-flux onset curve, autocorrelation, light prior around 120 BPM ----
  {
    constexpr std::size_t tN = 1024, tHop = 512;
    FFT tf(tN);
    const auto tw = hann(tN);
    std::vector<std::complex<double>> tb(tN);
    std::vector<double> prev(tN / 2, 0.0), env;
    const std::size_t nFrames = N >= tN ? (N - tN) / tHop + 1 : 0;
    for (std::size_t fr = 0; fr < nFrames; ++fr) {
      for (std::size_t i = 0; i < tN; ++i) tb[i] = 0.5 * (L[fr * tHop + i] + R[fr * tHop + i]) * tw[i];
      tf.forward(tb);
      double flux = 0;
      for (std::size_t k = 1; k < tN / 2; ++k) {
        const double m = std::log1p(100.0 * std::abs(tb[k]));
        flux += std::max(0.0, m - prev[k]);
        prev[k] = m;
      }
      env.push_back(flux);
    }
    if (env.size() > 200) {
      double mean = 0; for (double v : env) mean += v; mean /= static_cast<double>(env.size());
      for (double& v : env) v = std::max(0.0, v - mean);
      const double fps = sr / static_cast<double>(tHop);
      const int minLag = static_cast<int>(fps * 60.0 / 200.0), maxLag = static_cast<int>(fps * 60.0 / 60.0);
      std::vector<double> ac(static_cast<std::size_t>(maxLag + 2), 0.0);
      for (int lag = minLag - 1; lag <= maxLag + 1 && static_cast<std::size_t>(lag) < env.size(); ++lag) {
        double s = 0;
        for (std::size_t i = static_cast<std::size_t>(lag); i < env.size(); ++i) s += env[i] * env[i - static_cast<std::size_t>(lag)];
        ac[static_cast<std::size_t>(lag)] = s / static_cast<double>(env.size() - static_cast<std::size_t>(lag));
      }
      double best = 0, sumAc = 0; int bestLag = 0, cnt = 0;
      for (int lag = minLag; lag <= maxLag; ++lag) {
        const double bpm = 60.0 * fps / lag;
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 1.0, 2.0));
        const double v = ac[static_cast<std::size_t>(lag)] * (0.6 + 0.4 * prior);
        sumAc += ac[static_cast<std::size_t>(lag)]; ++cnt;
        if (v > best) { best = v; bestLag = lag; }
      }
      if (bestLag > 0 && cnt > 0 && sumAc > 0) {
        const double y0 = ac[static_cast<std::size_t>(bestLag - 1)], y1 = ac[static_cast<std::size_t>(bestLag)], y2 = ac[static_cast<std::size_t>(bestLag + 1)];
        const double den = y0 - 2 * y1 + y2;
        const double off = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
        f.bpm = static_cast<float>(60.0 * fps / (bestLag + std::clamp(off, -0.5, 0.5)));
        f.bpmConfidence = static_cast<float>(std::clamp(ac[static_cast<std::size_t>(bestLag)] / (sumAc / cnt) / 8.0, 0.0, 1.0));
      }
    }
  }
  return f;
}

const std::vector<std::string>& featureNames() {
  static const std::vector<std::string> n = [] {
    std::vector<std::string> v = {"lufs", "lra", "truePeak", "rms", "crest", "centroid", "width", "correlation", "bpm"};
    for (int b = 0; b < kBands; ++b) v.push_back(std::string("band") + std::to_string(b));
    return v;
  }();
  return n;
}
std::vector<float> toVector(const Features& f) {
  std::vector<float> v = {f.lufs, f.loudnessRangeLu, f.truePeakDb, f.rmsDb, f.crestDb, f.centroidHz, f.stereoWidth, f.correlation, f.bpm};
  for (float b : f.bandDb) v.push_back(b);
  return v;
}
Features fromVector(const std::vector<float>& v) {
  Features f;
  if (v.size() != featureNames().size()) return f;
  f.seconds = 1.0;
  f.lufs = v[0]; f.loudnessRangeLu = v[1]; f.truePeakDb = v[2]; f.rmsDb = v[3]; f.crestDb = v[4]; f.centroidHz = v[5];
  f.stereoWidth = v[6]; f.correlation = v[7]; f.bpm = v[8];
  for (int b = 0; b < kBands; ++b) f.bandDb[static_cast<std::size_t>(b)] = v[9 + static_cast<std::size_t>(b)];
  return f;
}

}  // namespace wezt::ai
