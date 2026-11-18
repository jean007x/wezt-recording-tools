#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

namespace wezt {

// Planar float audio. `stride` is the capacity of each channel; `frames` is how many of them are
// valid right now. A buffer allocated for the largest block can therefore carry any smaller block
// (device callbacks vary in size) without reallocating on the audio thread.
struct AudioBuffer {
  std::size_t channels = 0, frames = 0, stride = 0;
  std::vector<float> data;

  AudioBuffer() = default;
  AudioBuffer(std::size_t ch, std::size_t fr) : channels(ch), frames(fr), stride(fr), data(ch * fr, 0.f) {}

  float* channel(std::size_t c) { return data.data() + c * stride; }
  const float* channel(std::size_t c) const { return data.data() + c * stride; }

  std::size_t capacity() const { return stride; }
  // Changes the number of valid frames; never allocates. Clamped to capacity.
  void setFrames(std::size_t n) { frames = std::min(n, stride); }
  void clear() { std::fill(data.begin(), data.end(), 0.f); }
  void clearValid() {
    for (std::size_t c = 0; c < channels; ++c) std::fill(channel(c), channel(c) + frames, 0.f);
  }
  float peak() const {
    float p = 0.f;
    for (std::size_t c = 0; c < channels; ++c)
      for (std::size_t i = 0; i < frames; ++i) p = std::max(p, channel(c)[i] < 0 ? -channel(c)[i] : channel(c)[i]);
    return p;
  }
};

}  // namespace wezt
