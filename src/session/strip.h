#pragma once
// Mixer building blocks: fader/pan/mute strip with metering, automation lanes, dB helpers.
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/context.h"
#include "engine/graph.h"

namespace wezt::session {

inline float dbToLin(float db) { return db <= -90.f ? 0.f : std::pow(10.f, db / 20.f); }
inline float linToDb(float x) { return x <= 1e-5f ? -100.f : 20.f * std::log10(x); }

struct AutomationPoint {
  std::int64_t frame = 0;
  float value = 0.f;
  bool operator==(const AutomationPoint& o) const { return frame == o.frame && value == o.value; }
};

// Breakpoint automation. Linear between points (or held, for on/off values), flat before the first
// and after the last. Edit only while the transport is stopped; the audio thread only reads it.
class AutomationLane {
 public:
  explicit AutomationLane(bool stepped = false) : stepped_(stepped) {}
  void add(std::int64_t frame, float value) {  // replaces a point at the same frame
    if (frame < 0) frame = 0;
    auto it = std::lower_bound(pts_.begin(), pts_.end(), frame, [](const AutomationPoint& p, std::int64_t f) { return p.frame < f; });
    if (it != pts_.end() && it->frame == frame) it->value = value;
    else pts_.insert(it, {frame, value});
  }
  void clear() { pts_.clear(); }
  // Removes the points in [a, b].
  void eraseRange(std::int64_t a, std::int64_t b) {
    pts_.erase(std::remove_if(pts_.begin(), pts_.end(), [&](const AutomationPoint& p) { return p.frame >= a && p.frame <= b; }), pts_.end());
  }
  // Thin Automation: drops points that lie within `tolerance` of the line through their neighbours
  // (Douglas-Peucker). The first and last points stay. Stepped lanes are left alone.
  void thin(float tolerance) {
    if (stepped_ || pts_.size() < 3) return;
    std::vector<char> keep(pts_.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, pts_.size() - 1}};
    while (!stack.empty()) {
      auto [a, b] = stack.back();
      stack.pop_back();
      float worst = -1.f;
      std::size_t wi = a;
      for (std::size_t i = a + 1; i < b; ++i) {
        const double t = static_cast<double>(pts_[i].frame - pts_[a].frame) / static_cast<double>(pts_[b].frame - pts_[a].frame);
        const float line = static_cast<float>(pts_[a].value + (pts_[b].value - pts_[a].value) * t);
        const float d = std::fabs(pts_[i].value - line);
        if (d > worst) { worst = d; wi = i; }
      }
      if (worst > tolerance) { keep[wi] = 1; stack.push_back({a, wi}); stack.push_back({wi, b}); }
    }
    std::vector<AutomationPoint> out;
    for (std::size_t i = 0; i < pts_.size(); ++i) if (keep[i]) out.push_back(pts_[i]);
    pts_.swap(out);
  }
  bool empty() const { return pts_.empty(); }
  bool stepped() const { return stepped_; }
  const std::vector<AutomationPoint>& points() const { return pts_; }
  float valueAt(std::int64_t frame, float fallback) const {
    if (pts_.empty()) return fallback;
    if (frame <= pts_.front().frame) return pts_.front().value;
    if (frame >= pts_.back().frame) return pts_.back().value;
    auto hi = std::upper_bound(pts_.begin(), pts_.end(), frame, [](std::int64_t f, const AutomationPoint& p) { return f < p.frame; });
    const auto lo = hi - 1;
    if (stepped_) return lo->value;
    const double t = static_cast<double>(frame - lo->frame) / static_cast<double>(hi->frame - lo->frame);
    return static_cast<float>(lo->value + (hi->value - lo->value) * t);
  }

 private:
  std::vector<AutomationPoint> pts_;
  bool stepped_;
};

// Peak meter that is safe to read from the UI thread while the audio thread feeds it.
class PeakMeter {
 public:
  static constexpr std::size_t kMaxChannels = 8;
  void feed(std::size_t c, float p) {  // audio thread
    if (c >= kMaxChannels) return;
    float cur = peak_[c].load(std::memory_order_relaxed);
    while (p > cur && !peak_[c].compare_exchange_weak(cur, p, std::memory_order_relaxed)) {}
    if (p > 1.f) clipped_.store(true, std::memory_order_relaxed);
  }
  float takePeak(std::size_t c) { return c < kMaxChannels ? peak_[c].exchange(0.f, std::memory_order_relaxed) : 0.f; }  // peak since last call
  bool clipped() const { return clipped_.load(std::memory_order_relaxed); }
  void resetClip() { clipped_.store(false, std::memory_order_relaxed); }

 private:
  std::array<std::atomic<float>, kMaxChannels> peak_{};
  std::atomic<bool> clipped_{false};
};

// Fader + pan + mute for one track, bus or the master, with a post-fader meter.
//
// Pan: a mono source uses a constant-power law (-3 dB at centre); a stereo source uses balance
// (unity at centre, the far side is turned down). Gain changes ramp over the block, so there are
// no clicks. Fader, pan and mute follow their automation lanes while the transport plays and
// automation is on.
class Strip : public Processor {
 public:
  explicit Strip(bool monoSource = false) : monoSource_(monoSource), muteLane_(true) {}

  void setContext(const RtContext* c) { ctx_ = c; }
  void setGainDb(float db) { gainDb_.store(std::clamp(db, -100.f, 12.f), std::memory_order_relaxed); }
  void setPan(float p) { pan_.store(std::clamp(p, -1.f, 1.f), std::memory_order_relaxed); }
  void setMute(bool m) { mute_.store(m, std::memory_order_relaxed); }
  void setSilenced(bool s) { silenced_.store(s, std::memory_order_relaxed); }  // solo logic
  // Extra gain from VCA masters (dB, summed by the session). Added to the fader and its automation.
  void setVcaDb(float db) { vcaDb_.store(std::clamp(db, -100.f, 24.f), std::memory_order_relaxed); }
  float vcaDb() const { return vcaDb_.load(std::memory_order_relaxed); }
  void setAutomationOn(bool on) { automate_.store(on, std::memory_order_relaxed); }
  float gainDb() const { return gainDb_.load(std::memory_order_relaxed); }
  float pan() const { return pan_.load(std::memory_order_relaxed); }
  bool muted() const { return mute_.load(std::memory_order_relaxed); }
  bool silenced() const { return silenced_.load(std::memory_order_relaxed); }
  bool automationOn() const { return automate_.load(std::memory_order_relaxed); }
  bool monoSource() const { return monoSource_; }

  AutomationLane& volumeLane() { return volLane_; }  // values in dB
  AutomationLane& panLane() { return panLane_; }     // -1..1
  AutomationLane& muteLane() { return muteLane_; }   // 0 / 1
  PeakMeter& meter() { return meter_; }

  void prepare(double, std::size_t) override { first_ = true; }

  void process(const AudioBuffer& in, AudioBuffer& out) override {
    const std::size_t n = out.frames, ch = std::min(in.channels, out.channels);
    const bool stereo = out.channels >= 2;
    float tL, tR;
    targetsAt(ctx_ ? ctx_->chunkStart + static_cast<std::int64_t>(n) : 0, stereo, tL, tR);
    if (first_) { curL_ = tL; curR_ = tR; first_ = false; }
    const float sL = (tL - curL_) / static_cast<float>(n ? n : 1), sR = (tR - curR_) / static_cast<float>(n ? n : 1);
    for (std::size_t c = 0; c < ch; ++c) {
      const float* x = in.channel(c);
      float* y = out.channel(c);
      const bool right = stereo && c == 1;  // channels beyond stereo get the plain fader (left gain)
      const float g0 = right ? curR_ : curL_, st = right ? sR : sL;
      for (std::size_t i = 0; i < n; ++i) y[i] = x[i] * (g0 + st * static_cast<float>(i + 1));
    }
    for (std::size_t c = ch; c < out.channels; ++c) std::fill(out.channel(c), out.channel(c) + n, 0.f);
    curL_ = tL; curR_ = tR;
    for (std::size_t c = 0; c < out.channels; ++c) {
      float p = 0.f;
      const float* y = out.channel(c);
      for (std::size_t i = 0; i < n; ++i) p = std::max(p, std::fabs(y[i]));
      meter_.feed(c, p);
    }
  }

 private:
  void targetsAt(std::int64_t frame, bool stereo, float& gl, float& gr) const {
    float db = gainDb_.load(std::memory_order_relaxed), pan = pan_.load(std::memory_order_relaxed);
    bool mute = mute_.load(std::memory_order_relaxed);
    if (ctx_ && ctx_->playing && automate_.load(std::memory_order_relaxed)) {
      if (!volLane_.empty()) db = volLane_.valueAt(frame, db);
      if (!panLane_.empty()) pan = std::clamp(panLane_.valueAt(frame, pan), -1.f, 1.f);
      if (!muteLane_.empty()) mute = muteLane_.valueAt(frame, mute ? 1.f : 0.f) >= 0.5f;
    }
    db += vcaDb_.load(std::memory_order_relaxed);
    float g = (mute || silenced_.load(std::memory_order_relaxed)) ? 0.f : dbToLin(db);
    gl = gr = g;
    if (!stereo) return;
    if (monoSource_) {
      const float th = (pan + 1.f) * 0.78539816339744830962f;  // pi/4
      gl = g * std::cos(th);
      gr = g * std::sin(th);
    } else {
      gl = g * (pan > 0.f ? 1.f - pan : 1.f);
      gr = g * (pan < 0.f ? 1.f + pan : 1.f);
    }
  }

  const bool monoSource_;
  const RtContext* ctx_ = nullptr;
  std::atomic<float> gainDb_{0.f}, pan_{0.f}, vcaDb_{0.f};
  std::atomic<bool> mute_{false}, silenced_{false}, automate_{true};
  AutomationLane volLane_, panLane_, muteLane_;
  PeakMeter meter_;
  float curL_ = 1.f, curR_ = 1.f;
  bool first_ = true;
};

// Copies its input unchanged. Stands in for a plugin that could not be loaded so that the session
// still plays, and keeps the plugin's saved state so that saving again does not lose it.
class PassThrough : public Processor {
 public:
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    const std::size_t ch = std::min(in.channels, out.channels);
    for (std::size_t c = 0; c < ch; ++c) std::copy(in.channel(c), in.channel(c) + out.frames, out.channel(c));
    for (std::size_t c = ch; c < out.channels; ++c) std::fill(out.channel(c), out.channel(c) + out.frames, 0.f);
  }
};

}  // namespace wezt::session
