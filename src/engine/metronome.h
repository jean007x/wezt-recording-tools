// The click track: a short tick on every beat, louder on the first beat of the bar. It is mixed into the output after the graph,
// so it is heard but never recorded or bounced. Header-only (small, and used by the engine on the audio thread).
#pragma once
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace wezt {

class Metronome {
 public:
  // control thread
  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  void setCountOffBars(int bars) { countOff_.store(bars < 0 ? 0 : (bars > 8 ? 8 : bars), std::memory_order_relaxed); }
  void setTempo(double bpm) { tempo_.store(bpm < 20.0 ? 20.0 : (bpm > 400.0 ? 400.0 : bpm), std::memory_order_relaxed); }
  void setBeatsPerBar(int n) { beats_.store(n < 1 ? 1 : (n > 16 ? 16 : n), std::memory_order_relaxed); }
  void setLevel(float l) { level_.store(l < 0.f ? 0.f : (l > 1.f ? 1.f : l), std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  int countOffBars() const { return countOff_.load(std::memory_order_relaxed); }
  double tempo() const { return tempo_.load(std::memory_order_relaxed); }
  int beatsPerBar() const { return beats_.load(std::memory_order_relaxed); }
  double beatFrames(double sampleRate) const { return sampleRate * 60.0 / tempo(); }
  // How long a count-off lasts, in frames (0 when there is none).
  std::int64_t countOffFrames(double sampleRate) const {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(countOffBars()) * beatsPerBar() * beatFrames(sampleRate)));
  }

  // audio thread: adds the clicks that start in [pos, pos + frames) of the beat timeline to every channel.
  void mix(float* const* ch, std::size_t nch, std::size_t frames, std::int64_t pos, double sampleRate) {
    const double bf = beatFrames(sampleRate);
    const int bpb = beatsPerBar();
    const float level = level_.load(std::memory_order_relaxed) * 0.35f;
    const double end = static_cast<double>(pos) + static_cast<double>(frames);
    std::int64_t k = static_cast<std::int64_t>(std::ceil(static_cast<double>(pos) / bf - 1e-9));
    double nextAt = static_cast<double>(k) * bf;
    const double w1 = 2.0 * 3.14159265358979 * 1500.0 / sampleRate, w2 = 2.0 * 3.14159265358979 * 2250.0 / sampleRate;
    const double decay = std::exp(-1.0 / (0.012 * sampleRate));
    for (std::size_t i = 0; i < frames; ++i) {
      while (nextAt < end && static_cast<double>(pos) + static_cast<double>(i) >= nextAt - 0.5) {   // a beat starts here
        accent_ = (k % bpb) == 0; age_ = 0; amp_ = 1.0; ++k; nextAt = static_cast<double>(k) * bf;
      }
      if (age_ >= 0) {
        const double w = accent_ ? w2 : w1;
        const float v = static_cast<float>(std::sin(w * static_cast<double>(age_)) * amp_) * level * (accent_ ? 1.25f : 1.f);
        for (std::size_t c = 0; c < nch; ++c) ch[c][i] += v;
        amp_ *= decay;
        if (++age_ > static_cast<std::int64_t>(0.12 * sampleRate)) age_ = -1;
      }
    }
  }
  void reset() { age_ = -1; }

 private:
  std::atomic<bool> enabled_{false};
  std::atomic<int> countOff_{0}, beats_{4};
  std::atomic<double> tempo_{120.0};
  std::atomic<float> level_{0.8f};
  std::int64_t age_ = -1;   // audio thread: samples since the current click began (-1 = silent)
  double amp_ = 0.0;
  bool accent_ = false;
};

}  // namespace wezt
