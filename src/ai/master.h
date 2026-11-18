#pragma once
// Auto-master: plans a mastering chain (EQ, compressor, width, loudness, limiter) from what the
// advisor found, and renders it offline. After rendering it measures the result again and corrects
// the loudness and true peak, so what comes back is verified rather than assumed.
#include <vector>

#include "ai/advisor.h"
#include "dsp/effects.h"
#include "engine/audio_buffer.h"

namespace wezt::ai {

struct MasterSettings {
  std::vector<dsp::EqBand> eq;
  bool compress = false;
  dsp::CompressorParams comp;
  float widthSide = 1.f;        // side-signal scale
  float targetLufs = -14.f;
  float ceilingDb = -1.f;       // true-peak ceiling in dBTP
  bool limiter = true;
};

// Delivery loudness presets (integrated LUFS, true peak). These are common platform normalisation
// levels, not rules: the targets are adjustable.
struct DeliveryTarget { const char* name; float lufs; float truePeakDb; };
const std::vector<DeliveryTarget>& deliveryTargets();   // "streaming" -14/-1, "apple" -16/-1, "cd" -9/-0.3, "broadcast" -23/-1, "club" -8/-0.5

// Builds settings from suggestions. `accepted` selects which suggestions to apply (all when empty
// means use every one given). The loudness target is f.lufs + the loudness suggestion when there is
// one, otherwise `fallbackLufs`.
MasterSettings planMaster(const Features& f, const std::vector<Suggestion>& suggestions, float fallbackLufs, float ceilingDb = -1.f);

struct MasterResult {
  AudioBuffer audio;
  Features before, after;
  MasterSettings settings;   // as finally used (the pre-limiter gain is not part of it, see gainDb)
  float gainDb = 0.f;        // gain applied before the limiter to hit the loudness target
  float limiterCeilingDb = 0.f;
  int passes = 0;
  bool ok = false;
};

MasterResult applyMaster(const AudioBuffer& in, double sampleRate, const MasterSettings& s);

}  // namespace wezt::ai
