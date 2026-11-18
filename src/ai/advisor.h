#pragma once
// Turns "this track measures like X, the target measures like Y" into concrete, explainable
// suggestions, and learns from what the user does with them. Everything is plain arithmetic on the
// measurements in features.h, so every suggestion can say exactly why it was made.
#include <map>
#include <string>
#include <vector>

#include "ai/features.h"
#include "ai/profile.h"

namespace wezt::ai {

enum class SuggestionKind { Loudness, EqBand, Dynamics, Width };

struct Suggestion {
  SuggestionKind kind = SuggestionKind::Loudness;
  int band = -1;         // for EqBand
  float value = 0.f;     // Loudness: LU to add. EqBand: dB gain at the band. Dynamics: change of crest factor wanted (dB; negative = compress more). Width: change of side share.
  float confidence = 0.f;  // 0..1
  std::string text;      // plain-language reason
  std::string key() const;  // "loudness", "eq:3", "dynamics", "width"
};

// What the assistant has learned about this user's taste: nudges to every target and how often each
// kind of suggestion was taken. Persisted as a small text file.
struct Preferences {
  std::array<float, kBands> bandBias{};
  float loudnessBias = 0.f, crestBias = 0.f, widthBias = 0.f;
  std::map<std::string, std::pair<int, int>> record;  // key -> {accepted, offered}
  float acceptRate(const std::string& key) const;     // smoothed; 0.5 before any evidence
  std::string serialize() const;
  static bool parse(const std::string& text, Preferences& out);
};

struct AdviceOptions {
  float minEqDb = 1.5f;        // ignore smaller tonal differences
  float eqStrength = 0.6f;     // fraction of the difference suggested (a relative spectrum overstates it)
  float maxEqDb = 4.f;
  float minLoudnessLu = 1.f;
  float minCrestDb = 1.5f;
  float minWidth = 0.03f;
  float minConfidence = 0.25f;
};

// Compares `f` with the target profile (plus the user's learned bias). Strongest first.
std::vector<Suggestion> advise(const Features& f, const StyleProfile& target, const Preferences& pref, const AdviceOptions& opt = {});

// The user took (accepted, with the value they actually used) or declined a suggestion.
void learn(Preferences& pref, const Suggestion& s, bool accepted, float appliedValue, float rate = 0.3f);

}  // namespace wezt::ai
