#pragma once
// Reads a WAV or MP3 file into a plain stereo clip (used for the optional custom start-up sound).
// WAV: everything WavReader reads. MP3: decoded with minimp3 (public domain, third_party/minimp3).
#include <string>
#include <vector>

namespace wezt::io {

struct StereoClip {
  unsigned rate = 48000;
  std::vector<float> l, r;   // same length
  std::size_t frames() const { return l.size(); }
  double seconds() const { return rate ? static_cast<double>(l.size()) / rate : 0.0; }
};

// Loads at most `maxSeconds` of audio. Mono becomes dual mono, more than two channels keep the first two.
// Returns false with a plain-language reason in *error (unknown type, unreadable, damaged, empty).
bool loadStereoClip(const std::string& path, StereoClip& out, std::string* error, double maxSeconds = 30.0);

// Linear-interpolation sample rate conversion (good enough for a start-up sound).
StereoClip resampled(const StereoClip& in, unsigned newRate);

}  // namespace wezt::io
