#pragma once
// Headphone monitoring for a speaker layout: every speaker feed is placed at its direction with a
// spherical-head model (interaural time and level difference, head-shadow low-pass on the far ear).
// This is a simple model, good for checking a mix; it is not a measured-HRTF renderer.
#include <vector>

#include "engine/graph.h"
#include "immersive/layout.h"

namespace wezt::immersive {

class BinauralMonitor : public Processor {
 public:
  explicit BinauralMonitor(const SpeakerLayout& layout);
  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;  // in: one channel per speaker; out: 2 channels (L, R)
  std::size_t latencySamples() const override { return 0; }

 private:
  struct Path {
    float delay = 0.f;      // samples, far ear only
    bool farIsLeft = false;
    float farGain = 1.f, lpCoef = 1.f;
    bool lfe = false, center = false;  // centre-plane speakers go to both ears unfiltered
    std::vector<float> ring;  // delay line for the far ear
    std::size_t pos = 0;
    float lpState = 0.f;
  };
  SpeakerLayout layout_;
  std::vector<Path> paths_;
  double rate_ = 48000.0;
};

}  // namespace wezt::immersive
