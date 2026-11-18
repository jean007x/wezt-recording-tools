#pragma once
#include <cstddef>
#include <cstdint>

namespace wezt {

class Transport;

// What the engine tells processors about the chunk being rendered right now. It lives in the
// engine and is updated on the audio thread before every chunk; processors keep a pointer to it
// and read it only from inside process().
struct RtContext {
  const Transport* transport = nullptr;
  double sampleRate = 48000.0;
  std::int64_t chunkStart = 0;       // timeline frame of the first sample in this chunk
  std::size_t frames = 0;            // frames in this chunk
  bool playing = false;
  bool recording = false;
  unsigned inChannels = 0;           // channels the device really delivers
  const float* const* input = nullptr;
  const float* zeros = nullptr;      // at least maxBlock zeros, for missing channels
  std::int64_t recordLatencyFrames = 0;  // device output + input latency: shift takes earlier by this

  // Input channel `c` for this chunk; silence when the device has fewer channels.
  const float* inputChannel(unsigned c) const { return c < inChannels && input ? input[c] : zeros; }
};

}  // namespace wezt
