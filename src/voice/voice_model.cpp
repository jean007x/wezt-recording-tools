#include "voice/voice_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ai/fft.h"
#include "music/audio_to_midi.h"
#include "music/theory.h"

namespace wezt::voice {
namespace {
using Cx = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;
constexpr int kPts = 128;          // points of the log envelope
constexpr int kDim = kCep - 1;
constexpr double kMaxHz = 8000.0;

std::size_t nextPow2(std::size_t n) { std::size_t p = 1; while (p < n) p <<= 1; return p; }

double fmaxFor(double sr) { return std::min(kMaxHz, 0.45 * sr); }

// log envelope (natural log of magnitude) at frequency f, from cepstrum c
double envAt(const float* c, double f, double fmax) {
  double x = f / fmax * kPts - 0.5;
  double y;
  const double xc = std::min(x, kPts - 1.0);
  y = c[0];
  for (int k = 1; k < kCep; ++k) y += c[k] * std::cos(kPi * k * (xc + 0.5) / kPts);
  if (x > kPts - 1.0) y -= 0.5 * (f - fmax) / 1000.0;   // gentle fall beyond the analysed band
  return y;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))];
}

// k-means with deterministic initialisation. data: n x kDim.
std::vector<float> kmeans(const std::vector<std::vector<float>>& data, int K, int iters) {
  const std::size_t n = data.size();
  K = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(K), n));
  std::vector<float> book(static_cast<std::size_t>(K) * kDim);
  for (int k = 0; k < K; ++k) std::copy(data[static_cast<std::size_t>(k) * n / static_cast<std::size_t>(K)].begin(), data[static_cast<std::size_t>(k) * n / static_cast<std::size_t>(K)].end(), book.begin() + k * kDim);
  std::vector<int> assign(n, 0);
  for (int it = 0; it < iters; ++it) {
    bool changed = false;
    for (std::size_t i = 0; i < n; ++i) {
      int best = 0; double bd = 1e30;
      for (int k = 0; k < K; ++k) {
        double d = 0;
        for (int j = 0; j < kDim; ++j) { const double e = data[i][static_cast<std::size_t>(j)] - book[static_cast<std::size_t>(k * kDim + j)]; d += e * e; }
        if (d < bd) { bd = d; best = k; }
      }
      if (assign[i] != best) { assign[i] = best; changed = true; }
    }
    std::vector<double> sum(static_cast<std::size_t>(K) * kDim, 0.0);
    std::vector<int> cnt(static_cast<std::size_t>(K), 0);
    for (std::size_t i = 0; i < n; ++i) { ++cnt[static_cast<std::size_t>(assign[i])]; for (int j = 0; j < kDim; ++j) sum[static_cast<std::size_t>(assign[i] * kDim + j)] += data[i][static_cast<std::size_t>(j)]; }
    for (int k = 0; k < K; ++k) {
      if (cnt[static_cast<std::size_t>(k)] == 0) { const auto& r = data[(static_cast<std::size_t>(k) * 7919) % n]; std::copy(r.begin(), r.end(), book.begin() + k * kDim); continue; }   // re-seed an empty code
      for (int j = 0; j < kDim; ++j) book[static_cast<std::size_t>(k * kDim + j)] = static_cast<float>(sum[static_cast<std::size_t>(k * kDim + j)] / cnt[static_cast<std::size_t>(k)]);
    }
    if (!changed) break;
  }
  return book;
}

std::vector<float> meanOf(const std::vector<std::vector<float>>& d) {
  std::vector<float> m(kDim, 0.f);
  if (d.empty()) return m;
  for (auto& r : d) for (int j = 0; j < kDim; ++j) m[static_cast<std::size_t>(j)] += r[static_cast<std::size_t>(j)];
  for (auto& x : m) x /= static_cast<float>(d.size());
  return m;
}

}  // namespace

std::vector<VoiceFrame> analyzeVoice(const AudioBuffer& in, double sr) {
  std::vector<VoiceFrame> out;
  if (in.frames == 0 || in.channels == 0) return out;
  std::vector<float> x(in.frames, 0.f);
  for (std::size_t c = 0; c < in.channels; ++c) for (std::size_t i = 0; i < in.frames; ++i) x[i] += in.channel(c)[i] / static_cast<float>(in.channels);
  const std::size_t win = std::max<std::size_t>(64, static_cast<std::size_t>(0.032 * sr)), hop = std::max<std::size_t>(16, static_cast<std::size_t>(0.008 * sr));
  const std::size_t N = nextPow2(win) * 2;
  ai::FFT fft(N);
  std::vector<double> w(win);
  for (std::size_t i = 0; i < win; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(win));
  AudioBuffer mono(1, x.size());
  std::copy(x.begin(), x.end(), mono.channel(0));
  const auto pitch = music::trackPitch(mono, sr, 70, 1000);
  const double fmax = fmaxFor(sr);
  std::vector<Cx> spec(N);
  std::vector<double> y(kPts);
  std::size_t pi = 0;
  for (std::size_t start = 0; start + win <= x.size(); start += hop) {
    VoiceFrame f;
    f.time = static_cast<double>(start + win / 2) / sr;
    double e = 0;
    for (std::size_t i = 0; i < win; ++i) { e += static_cast<double>(x[start + i]) * x[start + i]; spec[i] = Cx(x[start + i] * w[i], 0.0); }
    for (std::size_t i = win; i < N; ++i) spec[i] = Cx(0, 0);
    f.rms = std::sqrt(e / static_cast<double>(win));
    f.silent = f.rms < 1e-3;
    while (pi + 1 < pitch.size() && std::fabs(pitch[pi + 1].time - f.time) <= std::fabs(pitch[pi].time - f.time)) ++pi;
    if (!pitch.empty() && std::fabs(pitch[pi].time - f.time) < 0.03 && pitch[pi].hz > 0 && pitch[pi].confidence > 0.5 && !f.silent) { f.voiced = true; f.f0 = pitch[pi].hz; }
    if (!f.silent) {
      fft.forward(spec);
      for (int n = 0; n < kPts; ++n) {
        const double fr = (n + 0.5) / kPts * fmax, pos = fr / sr * static_cast<double>(N);
        const std::size_t a = std::min<std::size_t>(N / 2 - 1, static_cast<std::size_t>(pos));
        const double t = pos - static_cast<double>(a);
        y[static_cast<std::size_t>(n)] = std::log(std::abs(spec[a]) * (1 - t) + std::abs(spec[a + 1]) * t + 1e-5);
      }
      for (int k = 0; k < kCep; ++k) {
        double s = 0;
        for (int n = 0; n < kPts; ++n) s += y[static_cast<std::size_t>(n)] * std::cos(kPi * k * (n + 0.5) / kPts);
        f.cep[k] = static_cast<float>(k == 0 ? s / kPts : 2.0 * s / kPts);
      }
    }
    out.push_back(f);
  }
  return out;
}

TrainResult trainVoiceModel(const std::vector<AudioBuffer>& recs, double sr, const TrainParams& p) {
  TrainResult r;
  if (!p.consent) { r.message = "A voice model can only be trained when you confirm that the voice is your own, or that the speaker has agreed."; return r; }
  if (p.name.empty()) { r.message = "Give the model a name."; return r; }
  std::vector<std::vector<float>> voiced, unvoiced;
  std::vector<double> f0s;
  double seconds = 0;
  const double hopSec = 0.008;
  for (const auto& a : recs) {
    for (const auto& f : analyzeVoice(a, sr)) {
      if (f.silent) continue;
      std::vector<float> v(f.cep + 1, f.cep + kCep);
      if (f.voiced) { voiced.push_back(std::move(v)); f0s.push_back(f.f0); seconds += hopSec; }
      else unvoiced.push_back(std::move(v));
    }
  }
  if (seconds < 20.0) {
    r.message = "I found only " + std::to_string(static_cast<int>(seconds)) + " seconds of clear voiced singing or speech. I need at least 20 seconds; a minute or two of dry, solo, close-miked voice with different vowels and pitches gives a much better model.";
    return r;
  }
  auto thin = [](std::vector<std::vector<float>>& d, std::size_t cap) {   // keep an even subset
    if (d.size() <= cap) return;
    std::vector<std::vector<float>> o;
    for (std::size_t i = 0; i < cap; ++i) o.push_back(std::move(d[i * d.size() / cap]));
    d.swap(o);
  };
  thin(voiced, 12000); thin(unvoiced, 4000);
  VoiceModel& m = r.model;
  m.name = p.name; m.owner = p.owner;
  m.consent = "The owner states the voice is their own or the speaker agreed to this model.";
  m.medianF0 = median(f0s); m.f0Low = percentile(f0s, 0.05); m.f0High = percentile(f0s, 0.95);
  m.voicedSeconds = seconds;
  m.meanVoiced = meanOf(voiced);
  m.voicedBook = kmeans(voiced, std::clamp(p.codes, 8, 256), std::clamp(p.iterations, 1, 50));
  m.codes = static_cast<int>(m.voicedBook.size() / kDim);
  if (unvoiced.size() >= 16) {
    m.meanUnvoiced = meanOf(unvoiced);
    m.unvoicedBook = kmeans(unvoiced, 12, 10);
    m.unvoicedCodes = static_cast<int>(m.unvoicedBook.size() / kDim);
  } else {
    m.meanUnvoiced = m.meanVoiced;
  }
  r.ok = true;
  r.message = "Trained \"" + m.name + "\" on " + std::to_string(static_cast<int>(seconds)) + " seconds of voiced sound: " + std::to_string(m.codes) + " timbre shapes, pitch range " +
              std::to_string(static_cast<int>(m.f0Low)) + "-" + std::to_string(static_cast<int>(m.f0High)) + " Hz (median " + std::to_string(static_cast<int>(m.medianF0)) + " Hz)." +
              (seconds < 90 ? " That is on the short side; more recordings with different vowels and pitches will sound better." : "");
  return r;
}

// ---- file ----
namespace {
template <class T> void put(std::vector<std::uint8_t>& b, T v) { const auto* p = reinterpret_cast<const std::uint8_t*>(&v); b.insert(b.end(), p, p + sizeof(T)); }
void putStr(std::vector<std::uint8_t>& b, const std::string& s) { put<std::uint32_t>(b, static_cast<std::uint32_t>(s.size())); b.insert(b.end(), s.begin(), s.end()); }
struct Reader {
  const std::vector<std::uint8_t>& b; std::size_t p = 0; bool bad = false;
  template <class T> T get() { T v{}; if (p + sizeof(T) > b.size()) { bad = true; return v; } std::memcpy(&v, b.data() + p, sizeof(T)); p += sizeof(T); return v; }
  std::string str() { const auto n = get<std::uint32_t>(); if (bad || n > 4096 || p + n > b.size()) { bad = true; return {}; } std::string s(reinterpret_cast<const char*>(b.data() + p), n); p += n; return s; }
  std::vector<float> floats(std::size_t n) { std::vector<float> v; if (bad || n > 1000000 || p + n * 4 > b.size()) { bad = true; return v; } v.resize(n); std::memcpy(v.data(), b.data() + p, n * 4); p += n * 4; return v; }
};
}  // namespace

bool saveVoiceModel(const std::string& path, const VoiceModel& m, std::string* error) {
  std::vector<std::uint8_t> b;
  const char magic[4] = {'W', 'V', 'M', '1'};
  b.insert(b.end(), magic, magic + 4);
  putStr(b, m.name); putStr(b, m.owner); putStr(b, m.consent);
  put<double>(b, m.medianF0); put<double>(b, m.f0Low); put<double>(b, m.f0High); put<double>(b, m.voicedSeconds);
  put<std::int32_t>(b, m.codes); put<std::int32_t>(b, m.unvoicedCodes);
  for (float f : m.meanVoiced) put<float>(b, f);
  for (float f : m.meanUnvoiced) put<float>(b, f);
  for (float f : m.voicedBook) put<float>(b, f);
  for (float f : m.unvoicedBook) put<float>(b, f);
  std::FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) { if (error) *error = "cannot write " + path; return false; }
  const bool ok = std::fwrite(b.data(), 1, b.size(), fp) == b.size();
  std::fclose(fp);
  if (!ok && error) *error = "write failed";
  return ok;
}

bool loadVoiceModel(const std::string& path, VoiceModel& m, std::string* error) {
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) { if (error) *error = "cannot open " + path; return false; }
  std::vector<std::uint8_t> b;
  std::uint8_t buf[4096]; std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0 && b.size() < (64u << 20)) b.insert(b.end(), buf, buf + n);
  std::fclose(fp);
  if (b.size() < 8 || std::memcmp(b.data(), "WVM1", 4) != 0) { if (error) *error = "not a Wezt voice model"; return false; }
  Reader r{b, 4};
  VoiceModel v;
  v.name = r.str(); v.owner = r.str(); v.consent = r.str();
  v.medianF0 = r.get<double>(); v.f0Low = r.get<double>(); v.f0High = r.get<double>(); v.voicedSeconds = r.get<double>();
  v.codes = r.get<std::int32_t>(); v.unvoicedCodes = r.get<std::int32_t>();
  if (r.bad || v.codes < 1 || v.codes > 256 || v.unvoicedCodes < 0 || v.unvoicedCodes > 64) { if (error) *error = "damaged model file"; return false; }
  v.meanVoiced = r.floats(kDim); v.meanUnvoiced = r.floats(kDim);
  v.voicedBook = r.floats(static_cast<std::size_t>(v.codes) * kDim);
  v.unvoicedBook = r.floats(static_cast<std::size_t>(v.unvoicedCodes) * kDim);
  if (r.bad) { if (error) *error = "damaged model file"; return false; }
  m = std::move(v);
  return true;
}

// ---- conversion ----
namespace {
void softShape(const std::vector<float>& book, int K, const float* shape, float* out) {
  // nearest four codes, weighted by inverse squared distance
  struct D { double d; int k; };
  std::vector<D> ds(static_cast<std::size_t>(K));
  for (int k = 0; k < K; ++k) {
    double d = 0;
    for (int j = 0; j < kDim; ++j) { const double e = shape[j] - book[static_cast<std::size_t>(k * kDim + j)]; d += e * e; }
    ds[static_cast<std::size_t>(k)] = {d, k};
  }
  const std::size_t M = std::min<std::size_t>(4, ds.size());
  std::partial_sort(ds.begin(), ds.begin() + static_cast<long>(M), ds.end(), [](const D& a, const D& b) { return a.d < b.d; });
  double wsum = 0;
  for (int j = 0; j < kDim; ++j) out[j] = 0.f;
  for (std::size_t i = 0; i < M; ++i) {
    const double w = 1.0 / (ds[i].d + 1e-3);
    wsum += w;
    for (int j = 0; j < kDim; ++j) out[j] += static_cast<float>(w * book[static_cast<std::size_t>(ds[static_cast<std::size_t>(i)].k * kDim + j)]);
  }
  for (int j = 0; j < kDim; ++j) out[j] = static_cast<float>(out[j] / wsum);
}
// Moves the formants: the envelope at frequency f becomes the source envelope at f / alpha.
void warpCep(const float* c, double alpha, double fmax, float* out) {
  double y[kPts];
  for (int n = 0; n < kPts; ++n) y[n] = envAt(c, (n + 0.5) / kPts * fmax / alpha, fmax);
  for (int k = 0; k < kCep; ++k) {
    double s = 0;
    for (int n = 0; n < kPts; ++n) s += y[n] * std::cos(kPi * k * (n + 0.5) / kPts);
    out[k] = static_cast<float>(k == 0 ? s / kPts : 2.0 * s / kPts);
  }
}

}  // namespace

AudioBuffer convertVoice(const AudioBuffer& in, double sr, const VoiceModel& m, const ConvertParams& p) {
  auto frames = analyzeVoice(in, sr);
  if (frames.size() < 4 || m.codes < 1) return {};
  const std::size_t hop = std::max<std::size_t>(16, static_cast<std::size_t>(0.008 * sr));
  const std::size_t win = std::max<std::size_t>(64, static_cast<std::size_t>(0.032 * sr));
  const double fmax = fmaxFor(sr);

  // source means (this recording), for the mean shift
  std::vector<std::vector<float>> sv, su;
  std::vector<double> f0s;
  for (auto& f : frames) {
    if (f.silent) continue;
    (f.voiced ? sv : su).push_back(std::vector<float>(f.cep + 1, f.cep + kCep));
    if (f.voiced) f0s.push_back(f.f0);
  }
  // Vocal-tract length: find the formant scaling that makes the source's envelopes fit the learned
  // shapes best (a longer tract has lower formants), and apply it to every frame.
  double alpha = 1.0;
  if (p.warpFormants && sv.size() >= 20 && m.codes > 0) {
    std::vector<const VoiceFrame*> sample;
    for (auto& f : frames) if (f.voiced && !f.silent) sample.push_back(&f);
    const std::size_t step = std::max<std::size_t>(1, sample.size() / 250);
    double best = 1e30;
    for (double a = 0.7; a <= 1.5001; a += 0.05) {
      std::vector<std::vector<float>> shapes;
      for (std::size_t i = 0; i < sample.size(); i += step) { float w[kCep]; warpCep(sample[i]->cep, a, fmax, w); shapes.emplace_back(w + 1, w + kCep); }
      const auto mu = meanOf(shapes);
      double tot = 0;
      for (auto& sh : shapes) {
        double bd = 1e30;
        for (int k = 0; k < m.codes; ++k) {
          double d = 0;
          for (int j = 0; j < kDim; ++j) { const double e = sh[static_cast<std::size_t>(j)] - mu[static_cast<std::size_t>(j)] + m.meanVoiced[static_cast<std::size_t>(j)] - m.voicedBook[static_cast<std::size_t>(k * kDim + j)]; d += e * e; }
          bd = std::min(bd, d);
        }
        tot += std::sqrt(bd);
      }
      if (tot < best) { best = tot; alpha = a; }
    }
  }
  if (std::fabs(alpha - 1.0) > 1e-6) {
    for (auto& f : frames) if (!f.silent) { float w[kCep]; warpCep(f.cep, alpha, fmax, w); std::copy(w, w + kCep, f.cep); }
    sv.clear(); su.clear();
    for (auto& f : frames) if (!f.silent) (f.voiced ? sv : su).push_back(std::vector<float>(f.cep + 1, f.cep + kCep));
  }
  const auto muSv = meanOf(sv), muSu = meanOf(su);
  double shift = p.pitchShiftSemitones;
  if (p.matchRange && !f0s.empty() && m.medianF0 > 0) shift += 12.0 * std::round(std::log2(m.medianF0 / median(f0s)));
  const double ratio = std::pow(2.0, shift / 12.0);

  // per-frame target envelope (as cepstra with c0 = 0: level comes from the source loudness)
  const std::size_t T = frames.size();
  std::vector<std::array<float, kCep>> env(T);
  for (std::size_t t = 0; t < T; ++t) {
    const VoiceFrame& f = frames[t];
    env[t].fill(0.f);
    if (f.silent) continue;
    const auto& muS = f.voiced ? muSv : muSu;
    const auto& muT = f.voiced ? m.meanVoiced : (m.unvoicedCodes ? m.meanUnvoiced : m.meanVoiced);
    float shifted[kDim], vq[kDim];
    for (int j = 0; j < kDim; ++j) shifted[j] = f.cep[j + 1] - muS[static_cast<std::size_t>(j)] + muT[static_cast<std::size_t>(j)];
    const auto& book = f.voiced ? m.voicedBook : m.unvoicedBook;
    const int K = f.voiced ? m.codes : m.unvoicedCodes;
    if (K > 0) {
      softShape(book, K, shifted, vq);
      for (int j = 0; j < kDim; ++j) env[t][static_cast<std::size_t>(j + 1)] = p.codebookWeight * vq[j] + (1.f - p.codebookWeight) * shifted[j];
    } else {
      for (int j = 0; j < kDim; ++j) env[t][static_cast<std::size_t>(j + 1)] = shifted[j];
    }
  }

  // harmonic part
  const std::size_t outLen = in.frames;
  std::vector<float> y(outLen + 2 * win, 0.f);
  auto centre = [&](std::size_t t) { return t * hop + win / 2; };
  const int maxH = 80;
  std::vector<double> theta(static_cast<std::size_t>(maxH) + 1);
  { music::Rng rng(11); for (auto& th : theta) th = rng.uniform() * 2.0 * kPi; }
  std::vector<std::vector<float>> amp(T, std::vector<float>(static_cast<std::size_t>(maxH) + 1, 0.f));
  std::vector<double> f0v(T, 0.0);
  double lastF0 = m.medianF0 > 0 ? m.medianF0 : 150.0;
  for (std::size_t t = 0; t < T; ++t) {
    const VoiceFrame& f = frames[t];
    if (f.voiced) lastF0 = f.f0 * ratio;
    f0v[t] = lastF0;
    if (!f.voiced) continue;
    double pw = 0;
    const int H = std::min(maxH, static_cast<int>(std::min(0.47 * sr, 12000.0) / f0v[t]));
    for (int h = 1; h <= H; ++h) { const double a = std::exp(envAt(env[t].data(), f0v[t] * h, fmax)); amp[t][static_cast<std::size_t>(h)] = static_cast<float>(a); pw += a * a * 0.5; }
    const double target = f.rms * std::sqrt(std::max(0.0, 1.0 - static_cast<double>(p.breath) * p.breath));
    const double g = pw > 0 ? target / std::sqrt(pw) : 0.0;
    for (int h = 1; h <= H; ++h) amp[t][static_cast<std::size_t>(h)] = static_cast<float>(amp[t][static_cast<std::size_t>(h)] * g);
  }
  double phase = 0;
  for (std::size_t t = 0; t + 1 < T; ++t) {
    for (std::size_t i = 0; i < hop; ++i) {
      const std::size_t pos = centre(t) + i;
      if (pos >= y.size()) break;
      const double u = static_cast<double>(i) / static_cast<double>(hop);
      const double f0 = f0v[t] * (1 - u) + f0v[t + 1] * u;
      phase += 2.0 * kPi * f0 / sr;
      if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
      double s = 0;
      const int H = std::min(maxH, static_cast<int>(std::min(0.47 * sr, 12000.0) / std::max(f0, 50.0)));
      for (int h = 1; h <= H; ++h) {
        const double a = amp[t][static_cast<std::size_t>(h)] * (1 - u) + amp[t + 1][static_cast<std::size_t>(h)] * u;
        if (a > 0) s += a * std::sin(h * phase + theta[static_cast<std::size_t>(h)]);
      }
      y[pos] += static_cast<float>(s);
    }
  }

  // noise part: shaped by the same envelope, overlap-added with 50% Hann blocks
  const std::size_t blk = 2 * hop, NF = nextPow2(blk);
  ai::FFT fft(NF);
  std::vector<Cx> spec(NF);
  music::Rng rng(23);
  for (std::size_t t = 0; t < T; ++t) {
    const VoiceFrame& f = frames[t];
    if (f.silent) continue;
    const double target = f.voiced ? f.rms * p.breath : f.rms;
    if (target < 1e-6) continue;
    for (std::size_t k = 0; k <= NF / 2; ++k) {
      const double fr = static_cast<double>(k) / static_cast<double>(NF) * sr;
      const double mag = std::exp(envAt(env[t].data(), fr, fmax));
      const double ph = rng.uniform() * 2.0 * kPi;
      spec[k] = std::polar(mag, ph);
      if (k > 0 && k < NF / 2) spec[NF - k] = std::conj(spec[k]);
    }
    spec[0] = Cx(spec[0].real(), 0.0); spec[NF / 2] = Cx(spec[NF / 2].real(), 0.0);
    for (auto& c : spec) c = std::conj(c);
    fft.forward(spec);
    std::vector<double> b(blk);
    double e = 0;
    for (std::size_t i = 0; i < blk; ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(blk));
      b[i] = spec[i].real() * w; e += b[i] * b[i];
    }
    const double rms = std::sqrt(e / static_cast<double>(blk) / 0.375);   // 0.375 = mean of Hann^2
    const double g = rms > 0 ? target / rms : 0.0;
    const std::size_t at = centre(t) - hop;
    for (std::size_t i = 0; i < blk && at + i < y.size(); ++i) y[at + i] += static_cast<float>(b[i] * g);
  }

  AudioBuffer out(1, outLen);
  float pk = 0.f;
  for (std::size_t i = 0; i < outLen; ++i) { out.channel(0)[i] = y[i]; pk = std::max(pk, std::fabs(y[i])); }
  if (pk > 0.98f) for (std::size_t i = 0; i < outLen; ++i) out.channel(0)[i] *= 0.98f / pk;
  return out;
}

}  // namespace wezt::voice
