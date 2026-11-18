#pragma once
// Audio analysis that runs entirely on this machine: loudness, dynamics, spectrum, stereo image,
// tempo and key. These numbers are what the local assistant "hears"; they are measurements, not a
// neural network, and are deterministic: the same audio always gives the same result.
#include <array>
#include <string>
#include <vector>

#include "engine/audio_buffer.h"

namespace wezt::ai {

constexpr int kBands = 10;  // octave bands centred on 31.5 63 125 250 500 1k 2k 4k 8k 16k Hz
extern const std::array<float, kBands> kBandCenters;
extern const char* const kBandNames[kBands];

struct Features {
  double seconds = 0;
  float lufs = -70.f;             // integrated, BS.1770-4
  float loudnessRangeLu = 0.f;    // spread of short-term loudness (p95 - p10), close to EBU Tech 3342
  float truePeakDb = -100.f;      // dBTP
  float rmsDb = -100.f;           // overall RMS of the mid signal
  float crestDb = 0.f;            // sample peak - RMS: how punchy / how squashed
  std::array<float, kBands> bandDb{};  // octave-band level relative to the total, dB (the tonal balance)
  float centroidHz = 0.f;         // spectral centre of mass (brightness)
  float stereoWidth = 0.f;        // side energy / (mid + side energy), 0 = mono, ~0.5 = very wide
  float correlation = 1.f;        // L/R correlation, -1..1
  float bpm = 0.f, bpmConfidence = 0.f;
  int keyRoot = -1;               // 0 = C ... 11 = B; -1 = unknown
  bool minor = false;
  float keyConfidence = 0.f;
  bool valid() const { return seconds > 0.0; }
};

// Analyses up to the first two channels. Returns an invalid Features for empty input.
Features analyze(const AudioBuffer& audio, double sampleRate);

std::string keyName(int root, bool minor);   // "A minor", "F# major", "?"
// Flat numeric view used by profiles: names and values in a fixed order.
const std::vector<std::string>& featureNames();
std::vector<float> toVector(const Features& f);
Features fromVector(const std::vector<float>& v);

}  // namespace wezt::ai
