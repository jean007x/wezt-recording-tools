#pragma once
// Looks at several tracks together and finds where they fight each other (for example a vocal and a
// guitar both sitting at 1 kHz, or too much low end from tracks that should not have any), proposes
// corrective EQ for the less important one, and can duck one track under another (sidechain).
//
// It measures; it does not guess at taste. Every proposal states what was measured, and nothing is
// applied until you accept it.
#include <string>
#include <vector>

#include "ai/features.h"
#include "dsp/effects.h"
#include "engine/audio_buffer.h"

namespace wezt::ai {

struct MixTrack {
  std::string name;
  const AudioBuffer* audio = nullptr;
};

enum class Role { Kick, Drums, Bass, Vocal, Lead, Keys, Pad, Guitar, Other };
Role guessRole(const std::string& trackName);     // from words in the name: "vox", "kick", "808", "pad" ...
int rolePriority(Role r);                         // higher wins a clash

struct Clash {
  std::string winner, loser;     // the track that keeps the space and the one that makes room
  int band = 0;                  // octave band index (see kBandNames)
  float winnerDb = 0, loserDb = 0;   // each track's level in that band, dB
};

struct MixFix {
  std::string track;
  std::string why;
  dsp::EqBand eq;
};

struct MixReport {
  std::vector<Clash> clashes;
  std::vector<MixFix> fixes;
  std::vector<std::string> notes;   // plain-language observations, e.g. low-end build-up
};

MixReport analyzeMix(const std::vector<MixTrack>& tracks, double sampleRate);
// Applies one fix offline to a copy of the audio.
AudioBuffer applyFix(const AudioBuffer& audio, double sampleRate, const MixFix& fix);

struct SidechainParams {
  float thresholdDb = -30.f;     // level of the key signal above which the target is reduced
  float ratio = 4.f;
  float attackMs = 5.f, releaseMs = 140.f;
  float maxReductionDb = 12.f;
};
// Reduces `target` by an amount that follows the loudness of `key` (e.g. bass ducking under the kick).
// The two may differ in length; the key is treated as silent past its end.
AudioBuffer sidechainCompress(const AudioBuffer& target, const AudioBuffer& key, double sampleRate, const SidechainParams& p = {});

}  // namespace wezt::ai
