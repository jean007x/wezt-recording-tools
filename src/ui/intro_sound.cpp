#include "ui/intro_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wezt::ui {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Rng {
  std::uint32_t s = 0x1234abcd;
  float bip() { s = s * 1664525u + 1013904223u; return static_cast<float>(s >> 8) * (1.f / 8388608.f) - 1.f; }   // -1..1
  float uni() { return bip() * 0.5f + 0.5f; }
};

struct OnePole {   // low-pass
  float z = 0;
  float run(float x, float a) { z += a * (x - z); return z; }
};
float cutoffCoef(double fc, double sr) { return static_cast<float>(1.0 - std::exp(-2.0 * kPi * std::min(fc, sr * 0.45) / sr)); }

struct Svf {   // Chamberlin state-variable filter, band-pass output
  float lo = 0, band = 0;
  float run(float x, double fc, double sr, float damp) {
    const float f = static_cast<float>(2.0 * std::sin(kPi * std::min(fc, sr * 0.25) / sr));
    lo += f * band;
    const float hi = x - lo - damp * band;
    band += f * hi;
    return band;
  }
};

struct Comb {
  std::vector<float> buf; std::size_t i = 0; float damp = 0;
  explicit Comb(std::size_t n) : buf(std::max<std::size_t>(1, n), 0.f) {}
  float run(float x, float g) { const float y = buf[i]; damp += 0.35f * (y - damp); buf[i] = x + damp * g; if (++i == buf.size()) i = 0; return y; }
};
struct Allpass {
  std::vector<float> buf; std::size_t i = 0;
  explicit Allpass(std::size_t n) : buf(std::max<std::size_t>(1, n), 0.f) {}
  float run(float x, float g) { const float d = buf[i]; const float y = -x + d; buf[i] = x + d * g; if (++i == buf.size()) i = 0; return y; }
};

float smooth(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }

// A saw built from a phase accumulator (aliasing is irrelevant here: it is low-passed hard).
struct Saw { double ph = 0; float run(double f, double sr) { ph += f / sr; if (ph >= 1.0) ph -= 1.0; return static_cast<float>(2.0 * ph - 1.0); } };

}  // namespace

io::StereoClip renderIntroSound(unsigned rateIn) {
  const double sr = rateIn ? rateIn : 48000;
  io::StereoClip out;
  out.rate = static_cast<unsigned>(sr);
  const std::size_t N = static_cast<std::size_t>((kIntroOpen + 3.2) * sr);
  std::vector<float> L(N, 0.f), R(N, 0.f);
  Rng rng;
  const auto at = [&](double sec) { return static_cast<std::size_t>(sec * sr); };

  // ---- drone: three detuned saw pairs (D1, A1, D2) swelling for 3.8 s, a low-pass opening as it goes ----
  {
    Saw saws[6]; const double base[6] = {36.71, 36.71 * 1.004, 55.0, 55.0 * 0.996, 73.42, 73.42 * 1.003};
    OnePole lp1, lp2;
    for (std::size_t i = 0; i < N; ++i) {
      const double t = static_cast<double>(i) / sr;
      const float amp = t < kIntroGrowEnd ? 0.50f * std::pow(static_cast<float>(t / kIntroGrowEnd), 1.6f)
                                           : 0.50f * std::exp(-static_cast<float>(t - kIntroGrowEnd) * 1.1f);
      if (amp < 1e-4f) continue;
      float s = 0; for (int k = 0; k < 6; ++k) s += saws[k].run(base[k], sr);
      const double fc = t < kIntroGrowEnd ? 90.0 + 1500.0 * std::pow(t / kIntroGrowEnd, 2.0) : 500.0;
      const float a = cutoffCoef(fc, sr);
      s = lp2.run(lp1.run(s, a), a) * (1.f / 6.f) * 2.2f;
      const float trem = 0.85f + 0.15f * static_cast<float>(std::sin(2.0 * kPi * 5.2 * t));
      L[i] += s * amp * trem; R[i] += s * amp * trem;
    }
  }
  // ---- riser: band-passed noise sweeping 200 Hz -> 6 kHz, plus rising shimmer tones ----
  {
    Svf fl, fr; double ph[6] = {0, 0, 0, 0, 0, 0}; const double mul[6] = {1, 1.5, 2, 3, 4.01, 5.02};
    const std::size_t end = at(kIntroGrowEnd);
    for (std::size_t i = 0; i < end && i < N; ++i) {
      const double t = static_cast<double>(i) / sr, u = t / kIntroGrowEnd;
      const float amp = static_cast<float>(std::pow(u, 3.0)) * 0.42f * (1.f - smooth(static_cast<float>(kIntroGrowEnd - 0.03), static_cast<float>(kIntroGrowEnd), static_cast<float>(t)));
      const double fc = 200.0 * std::pow(30.0, u);
      L[i] += fl.run(rng.bip(), fc, sr, 0.22f) * amp * 0.9f;
      R[i] += fr.run(rng.bip(), fc * 1.01, sr, 0.22f) * amp * 0.9f;
      float sh = 0;
      for (int k = 0; k < 6; ++k) { ph[k] += 2.0 * kPi * 220.0 * mul[k] * std::pow(4.0, u) / sr; sh += static_cast<float>(std::sin(ph[k])); }
      const float sa = static_cast<float>(u * u) * 0.05f;
      L[i] += sh * sa; R[i] += sh * sa * 0.9f;
    }
  }
  // ---- impact at 3.8 s: sub boom, noise burst, a brassy chord ----
  {
    const std::size_t s0 = at(kIntroGrowEnd);
    double ph = 0; OnePole nlp; Saw sw[8]; const double f[8] = {73.42, 73.42 * 1.004, 110.0, 110.0 * 0.996, 146.83, 146.83 * 1.003, 220.0, 220.0 * 0.997};
    OnePole b1, b2;
    for (std::size_t i = s0; i < N; ++i) {
      const double tt = static_cast<double>(i - s0) / sr;
      if (tt > 5.0) break;
      ph += 2.0 * kPi * (38.0 + 60.0 * std::exp(-tt * 9.0)) / sr;
      float boom = static_cast<float>(std::sin(ph)) * 0.95f * std::exp(-static_cast<float>(tt) * 1.6f);
      float nz = nlp.run(rng.bip(), cutoffCoef(900, sr)) * 0.9f * std::exp(-static_cast<float>(tt) * 9.f);
      float br = 0; for (int k = 0; k < 8; ++k) br += sw[k].run(f[k], sr);
      const float a = cutoffCoef(900, sr);
      br = b2.run(b1.run(br, a), a) * 0.30f * (1.f - std::exp(-static_cast<float>(tt) * 200.f)) * std::exp(-static_cast<float>(tt) * 1.3f);
      const float v = boom + nz + br;
      L[i] += v; R[i] += v;
    }
  }
  // ---- descent 4.5 -> 6.0 s: a falling whoosh that is cut off right where the clash lands ----
  {
    Svf fl, fr; double ph = 0;
    const std::size_t s0 = at(kIntroHoldEnd), s1 = at(kIntroOpen);
    for (std::size_t i = s0; i < s1 && i < N; ++i) {
      const double v = static_cast<double>(i - s0) / static_cast<double>(s1 - s0);
      const float cut = 1.f - smooth(0.985f, 1.f, static_cast<float>(v));
      const float amp = static_cast<float>(std::pow(v, 1.5)) * 0.40f * cut;
      const double fc = 7000.0 * std::pow(0.04, v);
      L[i] += fl.run(rng.bip(), fc, sr, 0.2f) * amp;
      R[i] += fr.run(rng.bip(), fc * 0.99, sr, 0.2f) * amp;
      ph += 2.0 * kPi * (1200.0 * std::pow(80.0 / 1200.0, v)) / sr;
      const float s = static_cast<float>(std::sin(ph)) * static_cast<float>(v * v) * 0.18f * cut;
      L[i] += s; R[i] += s;
    }
  }
  // ---- the clash at 6.0 s: several quick strikes of steel on steel (inharmonic ringing partials, a bright transient, a scrape) ----
  {
    static const double ratio[14] = {1, 1.47, 1.96, 2.41, 2.93, 3.38, 4.12, 4.87, 5.43, 6.31, 7.04, 7.88, 8.9, 10.2};
    struct Strike { double at, base, gain; };
    const Strike strikes[4] = {{0.0, 410, 1.0}, {0.038, 560, 0.8}, {0.095, 335, 0.65}, {0.17, 690, 0.5}};
    for (const Strike& st : strikes) {
      const std::size_t s0 = at(kIntroOpen + st.at);
      struct Part { double f, ph, tau; float a, panL, panR; };
      Part parts[14];
      for (int k = 0; k < 14; ++k) {
        const float pan = rng.uni();
        parts[k] = {st.base * ratio[k] * (1.0 + 0.004 * rng.bip()), 0.0, (0.9 + 1.6 * rng.uni()) / (1.0 + 0.18 * k),
                    static_cast<float>(st.gain) * (0.5f + 0.5f * rng.uni()) / (1.f + 0.5f * static_cast<float>(k)) * 0.55f,
                    std::cos(pan * 1.5708f), std::sin(pan * 1.5708f)};
      }
      OnePole hp;
      for (std::size_t i = s0; i < N; ++i) {
        const double tt = static_cast<double>(i - s0) / sr;
        if (tt > 4.0) break;
        float l = 0, r = 0;
        for (Part& p : parts) {
          p.ph += 2.0 * kPi * p.f / sr;
          const float e = p.a * static_cast<float>(std::exp(-tt / p.tau)) * static_cast<float>(std::sin(p.ph));
          l += e * p.panL; r += e * p.panR;
        }
        const float n = rng.bip();
        const float tr = (n - hp.run(n, cutoffCoef(2500, sr))) * static_cast<float>(st.gain) * 0.8f * std::exp(-static_cast<float>(tt) * 70.f);   // bright tick: high-passed noise
        L[i] += l + tr; R[i] += r + tr;
      }
    }
    Svf fl, fr; const std::size_t s0 = at(kIntroOpen);
    for (std::size_t i = s0; i < N; ++i) {   // grinding scrape, wobbling at ~38 Hz
      const double tt = static_cast<double>(i - s0) / sr;
      if (tt > 0.6) break;
      const float amp = 0.30f * std::exp(-static_cast<float>(tt) * 6.f) * (0.6f + 0.4f * static_cast<float>(std::sin(2.0 * kPi * 38.0 * tt)));
      const double fc = 3200.0 + 1500.0 * std::sin(2.0 * kPi * 7.0 * tt);
      L[i] += fl.run(rng.bip(), fc, sr, 0.12f) * amp;
      R[i] += fr.run(rng.bip(), fc * 1.02, sr, 0.12f) * amp;
    }
  }
  // ---- a hall: four damped combs and two all-passes per side, mixed in under the dry sound ----
  {
    const double dl[4] = {0.0297, 0.0371, 0.0411, 0.0437}, dr[4] = {0.0311, 0.0389, 0.0427, 0.0463};
    std::vector<Comb> cl, cr;
    for (int k = 0; k < 4; ++k) { cl.emplace_back(static_cast<std::size_t>(dl[k] * sr)); cr.emplace_back(static_cast<std::size_t>(dr[k] * sr)); }
    Allpass al1(static_cast<std::size_t>(0.005 * sr)), al2(static_cast<std::size_t>(0.0017 * sr)), ar1(static_cast<std::size_t>(0.0053 * sr)), ar2(static_cast<std::size_t>(0.0019 * sr));
    for (std::size_t i = 0; i < N; ++i) {
      const float inL = L[i] * 0.25f, inR = R[i] * 0.25f;
      float wl = 0, wr = 0;
      for (int k = 0; k < 4; ++k) { wl += cl[static_cast<std::size_t>(k)].run(inL + inR * 0.3f, 0.86f); wr += cr[static_cast<std::size_t>(k)].run(inR + inL * 0.3f, 0.86f); }
      wl = al2.run(al1.run(wl, 0.7f), 0.7f); wr = ar2.run(ar1.run(wr, 0.7f), 0.7f);
      L[i] += wl * 0.55f; R[i] += wr * 0.55f;
    }
  }
  // ---- master: soft clip, normalize, short fade in and a long fade out ----
  float peak = 0;
  for (std::size_t i = 0; i < N; ++i) {
    L[i] = std::tanh(L[i] * 1.2f); R[i] = std::tanh(R[i] * 1.2f);
    peak = std::max({peak, std::fabs(L[i]), std::fabs(R[i])});
  }
  const float norm = peak > 0.f ? 0.89f / peak : 1.f;
  const std::size_t fin = at(0.02), fout = at(1.5);
  for (std::size_t i = 0; i < N; ++i) {
    float g = norm;
    if (i < fin) g *= static_cast<float>(i) / static_cast<float>(fin);
    if (i + fout > N) g *= 0.5f - 0.5f * std::cos(3.14159265f * static_cast<float>(N - i) / static_cast<float>(fout));   // smooth, so the tail dies without a corner
    L[i] *= g; R[i] *= g;
  }
  out.l = std::move(L); out.r = std::move(R);
  return out;
}

// ------------------------------------------------------------------------------------------------------------------

IntroPlayer::~IntroPlayer() { release(); }

void IntroPlayer::release() {
  if (dev_) { dev_->stop(); dev_.reset(); }
  done_ = true;
}

bool IntroPlayer::start(const std::string& customPath, std::string* message) {
  release();
  dev_ = audio::createPlatformDevice();
  if (!dev_) { if (message) *message = "no audio output on this system"; return false; }
  audio::AudioConfig cfg;
  cfg.sampleRate = 48000; cfg.bufferFrames = 512; cfg.inChannels = 0; cfg.outChannels = 2;
  std::string err;
  if (!dev_->open(cfg, this, &err)) { if (message) *message = err; dev_.reset(); return false; }
  const audio::AudioConfig act = dev_->actualConfig();
  const unsigned rate = act.sampleRate ? act.sampleRate : 48000;
  outCh_ = std::max(1u, act.outChannels);
  bool custom = false;
  if (!customPath.empty()) {
    io::StereoClip c; std::string e;
    if (io::loadStereoClip(customPath, c, &e)) { clip_ = io::resampled(c, rate); custom = true; }
    else if (message) *message = "Could not use your intro sound (" + e + "); playing the built-in one.";
  }
  if (!custom) clip_ = renderIntroSound(rate);
  // A custom sound gets a short fade-out at its end so a clip cut at the 30 s limit does not click.
  const std::size_t tail = std::min<std::size_t>(clip_.l.size(), rate / 10);
  for (std::size_t i = 0; i < tail; ++i) { const float g = static_cast<float>(i) / static_cast<float>(tail); clip_.l[clip_.l.size() - 1 - i] *= g; clip_.r[clip_.r.size() - 1 - i] *= g; }
  pos_ = 0; fading_ = false; fadeLeft_ = 0; fadeTotal_ = 1; done_ = false;
  if (!dev_->start(&err)) { if (message) *message = err; release(); return false; }
  return true;
}

void IntroPlayer::stop(double fadeSeconds) {
  if (done_ || fading_) return;
  const unsigned rate = std::max(1u, clip_.rate);
  fadeTotal_ = std::max<std::size_t>(1, static_cast<std::size_t>(fadeSeconds * rate));
  fadeLeft_ = fadeTotal_;
  fading_ = true;
}

bool IntroPlayer::playing() {
  if (done_ && dev_) release();
  if (dev_ && dev_->lost()) release();
  return !done_;
}

void IntroPlayer::process(const float* const* in, float* const* out, unsigned frames) {
  (void)in;
  for (unsigned c = 0; c < outCh_; ++c) std::fill(out[c], out[c] + frames, 0.f);
  if (done_) return;
  std::size_t p = pos_.load(std::memory_order_relaxed);
  const std::size_t n = clip_.l.size();
  std::size_t fl = fadeLeft_.load(std::memory_order_relaxed);
  const bool fading = fading_.load(std::memory_order_acquire);
  for (unsigned i = 0; i < frames; ++i) {
    if (p >= n) { done_ = true; break; }
    float g = 1.f;
    if (fading) {
      if (fl == 0) { done_ = true; break; }
      g = static_cast<float>(fl) / static_cast<float>(fadeTotal_);
      --fl;
    }
    out[0][i] = clip_.l[p] * g;
    if (outCh_ > 1) out[1][i] = clip_.r[p] * g;
    ++p;
  }
  pos_.store(p, std::memory_order_relaxed);
  fadeLeft_.store(fl, std::memory_order_relaxed);
}

}  // namespace wezt::ui
