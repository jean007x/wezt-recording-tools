#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

#include "engine/audio_buffer.h"

namespace wezt {

// Fixed delay for plugin delay compensation. Per channel circular buffer; allocation happens in
// setDelay() (never call it on the audio thread), process() is allocation-free.
class DelayLine {
 public:
  void setDelay(std::size_t samples, std::size_t channels) {
    if (samples == delay_ && channels == ch_) return;
    delay_ = samples;
    ch_ = channels;
    buf_.assign(channels * (samples + 1), 0.f);
    pos_ = 0;
  }
  std::size_t delay() const { return delay_; }
  void reset() { std::fill(buf_.begin(), buf_.end(), 0.f); pos_ = 0; }

  // In place. With zero delay this is a no-op.
  void process(AudioBuffer& b) {
    if (delay_ == 0) return;
    const std::size_t len = delay_ + 1, n = std::min(b.channels, ch_);
    for (std::size_t c = 0; c < n; ++c) {
      float* line = buf_.data() + c * len;
      float* x = b.channel(c);
      std::size_t p = pos_;
      for (std::size_t i = 0; i < b.frames; ++i) {
        line[p] = x[i];
        p = (p + 1 == len) ? 0 : p + 1;
        x[i] = line[p];  // oldest sample = input from `delay_` samples ago
      }
    }
    pos_ = (pos_ + b.frames) % len;
  }

 private:
  std::vector<float> buf_;
  std::size_t delay_ = 0, ch_ = 0, pos_ = 0;
};

}  // namespace wezt
