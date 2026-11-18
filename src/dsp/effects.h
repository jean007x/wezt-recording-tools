#pragma once
// Built-in processors used by the mastering chain and available as inserts: a parametric EQ, a
// compressor, a look-ahead limiter and a mid/side width control. They are plain Processors, so
// they run live in the graph and offline on a buffer with the same code.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <vector>

#include "dsp/biquad.h"
#include "engine/graph.h"

namespace wezt::dsp {

inline float dbToGain(float db) { return std::pow(10.f, db / 20.f); }
inline float gainToDb(float g) { return g <= 1e-9f ? -180.f : 20.f * std::log10(g); }

struct EqBand {
  FilterType type = FilterType::Peaking;
  float freq = 1000.f, gainDb = 0.f, q = 1.f;
  bool enabled = true;
};

// Up to 12 bands in series. Bands can be changed from the control thread at any time: the audio
// thread picks the new set up at the next block and never waits.
class ParametricEq : public Processor {
 public:
  static constexpr std::size_t kMaxBands = 12;
  ParametricEq() { pending_.reserve(kMaxBands); active_.reserve(kMaxBands); for (auto& f : filt_) f.reserve(kMaxBands); }   // so the audio thread never allocates
  void setBands(std::vector<EqBand> b) {
    if (b.size() > kMaxBands) b.resize(kMaxBands);
    while (lock_.test_and_set(std::memory_order_acquire)) {}
    pending_.assign(b.begin(), b.end());
    dirty_.store(true, std::memory_order_release);
    lock_.clear(std::memory_order_release);
  }
  // For the audio thread: no allocation (the pending list keeps room for kMaxBands) and no waiting.
  bool trySetBands(const EqBand* b, std::size_t n) {
    if (n > kMaxBands || lock_.test_and_set(std::memory_order_acquire)) return false;
    pending_.assign(b, b + n);
    dirty_.store(true, std::memory_order_release);
    lock_.clear(std::memory_order_release);
    return true;
  }
  void prepare(double sr, std::size_t) override { rate_ = sr; for (auto& ch : filt_) for (auto& f : ch) f.reset(); dirty_.store(true); }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    if (dirty_.load(std::memory_order_acquire) && !lock_.test_and_set(std::memory_order_acquire)) {
      active_ = pending_;
      dirty_.store(false, std::memory_order_release);
      lock_.clear(std::memory_order_release);
      for (auto& ch : filt_) {
        ch.resize(active_.size());
        for (std::size_t i = 0; i < active_.size(); ++i) ch[i].c = BiquadCoeffs::design(active_[i].type, rate_, active_[i].freq, active_[i].enabled ? active_[i].gainDb : 0.0, active_[i].q);
      }
    }
    const std::size_t ch = std::min(in.channels, out.channels), n = out.frames;
    for (std::size_t c = 0; c < ch; ++c) {
      const float* x = in.channel(c);
      float* y = out.channel(c);
      auto& fl = filt_[std::min<std::size_t>(c, 1)];
      for (std::size_t i = 0; i < n; ++i) {
        float v = x[i];
        for (auto& f : fl) v = f.run(v);
        y[i] = v;
      }
    }
    for (std::size_t c = ch; c < out.channels; ++c) std::fill(out.channel(c), out.channel(c) + n, 0.f);
  }

 private:
  double rate_ = 48000;
  std::vector<EqBand> pending_, active_;
  std::atomic<bool> dirty_{false};
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  std::vector<Biquad> filt_[2];  // channels beyond the second reuse the right channel's state
};

struct CompressorParams {
  float thresholdDb = -18.f, ratio = 2.f, attackMs = 20.f, releaseMs = 200.f, kneeDb = 6.f, makeupDb = 0.f, mix = 1.f;
};

// Feed-forward compressor with a stereo-linked peak detector and a soft knee.
class Compressor : public Processor {
 public:
  void set(const CompressorParams& p) { p_ = p; }
  void prepare(double sr, std::size_t) override { rate_ = sr; env_ = 0.f; }
  float lastGainReductionDb() const { return lastGr_; }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    const CompressorParams p = p_;
    const std::size_t ch = std::min(in.channels, out.channels), n = out.frames;
    const float aA = std::exp(-1.f / (std::max(0.01f, p.attackMs) * 0.001f * static_cast<float>(rate_)));
    const float aR = std::exp(-1.f / (std::max(0.01f, p.releaseMs) * 0.001f * static_cast<float>(rate_)));
    const float makeup = dbToGain(p.makeupDb);
    const float ratio = std::max(1.f, p.ratio);
    for (std::size_t i = 0; i < n; ++i) {
      float level = 0.f;
      for (std::size_t c = 0; c < ch; ++c) level = std::max(level, std::fabs(in.channel(c)[i]));
      const float lvDb = gainToDb(level);
      // gain computer with soft knee: output level for input level lvDb
      float over = lvDb - p.thresholdDb, grDb = 0.f;
      const float k = std::max(0.f, p.kneeDb);
      if (k > 0.f && std::fabs(over) <= k * 0.5f) { const float t = over + k * 0.5f; grDb = (1.f / ratio - 1.f) * t * t / (2.f * k); }
      else if (over > 0.f) grDb = (1.f / ratio - 1.f) * over;
      // smooth the reduction (attack when it grows, release when it shrinks)
      const float target = -grDb;  // positive reduction amount
      env_ = target > env_ ? aA * env_ + (1.f - aA) * target : aR * env_ + (1.f - aR) * target;
      lastGr_ = env_;
      const float g = dbToGain(-env_) * makeup;
      for (std::size_t c = 0; c < ch; ++c) {
        const float x = in.channel(c)[i];
        out.channel(c)[i] = x * (1.f - p.mix) + x * g * p.mix;
      }
    }
    for (std::size_t c = ch; c < out.channels; ++c) std::fill(out.channel(c), out.channel(c) + n, 0.f);
  }

 private:
  CompressorParams p_;
  double rate_ = 48000;
  float env_ = 0.f, lastGr_ = 0.f;
};

struct LimiterParams { float ceilingDb = -1.f, releaseMs = 80.f, lookaheadMs = 3.f; };

// Look-ahead limiter. The gain is a running minimum over the look-ahead window, released slowly and
// smoothed by a moving average of the same length, so it is already down when the peak arrives and
// the sample peak never exceeds the ceiling. Reports its delay through latencySamples().
class Limiter : public Processor {
 public:
  void set(const LimiterParams& p) { p_ = p; }
  void prepare(double sr, std::size_t) override {
    rate_ = sr;
    L_ = std::max<std::size_t>(1, static_cast<std::size_t>(p_.lookaheadMs * 0.001 * sr));
    for (auto& d : delay_) d.assign(L_, 0.f);
    req_.assign(L_ + 1, 1.f); env_hist_.assign(L_, 1.f);
    pos_ = 0; env_ = 1.f; avgSum_ = static_cast<double>(L_);
  }
  std::size_t latencySamples() const override { return L_; }
  float lastGainReductionDb() const { return lastGr_; }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    const std::size_t ch = std::min<std::size_t>(std::min(in.channels, out.channels), 2), n = out.frames;
    const float ceil = dbToGain(p_.ceilingDb);
    const float rel = 1.f - std::exp(-1.f / (std::max(1.f, p_.releaseMs) * 0.001f * static_cast<float>(rate_)));
    for (std::size_t i = 0; i < n; ++i) {
      float peak = 0.f;
      for (std::size_t c = 0; c < ch; ++c) peak = std::max(peak, std::fabs(in.channel(c)[i]));
      const float r = peak > ceil ? ceil / peak : 1.f;
      req_[pos_ % (L_ + 1)] = r;
      float mn = 1.f;
      for (std::size_t k = 0; k <= L_; ++k) mn = std::min(mn, req_[k]);  // window of L+1 requests, newest included
      env_ = mn < env_ ? mn : env_ + (mn - env_) * rel;
      env_ = std::min(env_, mn);
      const std::size_t hp = pos_ % L_;
      avgSum_ += env_ - env_hist_[hp];
      env_hist_[hp] = env_;
      const float g = static_cast<float>(avgSum_ / static_cast<double>(L_));
      lastGr_ = -gainToDb(g);
      for (std::size_t c = 0; c < ch; ++c) {
        const float d = delay_[c][hp];
        delay_[c][hp] = in.channel(c)[i];
        float y = d * g;
        y = std::clamp(y, -ceil, ceil);  // safety net for rounding; the smoothing normally keeps it inside already
        out.channel(c)[i] = y;
      }
      ++pos_;
    }
    for (std::size_t c = ch; c < out.channels; ++c) std::fill(out.channel(c), out.channel(c) + n, 0.f);
  }

 private:
  LimiterParams p_;
  double rate_ = 48000;
  std::size_t L_ = 144, pos_ = 0;
  std::vector<float> delay_[2], req_, env_hist_;
  float env_ = 1.f, lastGr_ = 0.f;
  double avgSum_ = 0;
};

// Mid/side width: scales the side signal. 1 = unchanged, 0 = mono, >1 wider.
class StereoWidth : public Processor {
 public:
  void setWidth(float w) { w_.store(std::clamp(w, 0.f, 2.f), std::memory_order_relaxed); }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    const float w = w_.load(std::memory_order_relaxed);
    if (in.channels < 2 || out.channels < 2) {
      for (std::size_t c = 0; c < std::min(in.channels, out.channels); ++c) std::copy(in.channel(c), in.channel(c) + out.frames, out.channel(c));
      return;
    }
    for (std::size_t i = 0; i < out.frames; ++i) {
      const float m = 0.5f * (in.channel(0)[i] + in.channel(1)[i]), s = 0.5f * (in.channel(0)[i] - in.channel(1)[i]) * w;
      out.channel(0)[i] = m + s;
      out.channel(1)[i] = m - s;
    }
    for (std::size_t c = 2; c < std::min(in.channels, out.channels); ++c) std::copy(in.channel(c), in.channel(c) + out.frames, out.channel(c));
  }

 private:
  std::atomic<float> w_{1.f};
};

}  // namespace wezt::dsp
