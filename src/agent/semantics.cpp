#include "agent/semantics.h"

#include <algorithm>
#include <cctype>

namespace wezt::agent {

std::string normalizeName(const std::string& s) {
  std::string o;
  for (unsigned char c : s) if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
  return o;
}

namespace {
bool hasWord(const std::string& lowerName, const char* w) {
  // whole-word match in a name split on non-letters, so "q" does not match "quality"
  const std::string needle = w;
  std::size_t pos = 0;
  while ((pos = lowerName.find(needle, pos)) != std::string::npos) {
    const bool left = pos == 0 || !std::isalpha(static_cast<unsigned char>(lowerName[pos - 1]));
    const std::size_t e = pos + needle.size();
    const bool right = e >= lowerName.size() || !std::isalpha(static_cast<unsigned char>(lowerName[e]));
    if (left && right) return true;
    pos = e;
  }
  return false;
}
bool has(const std::string& n, const char* sub) { return n.find(sub) != std::string::npos; }
}  // namespace

std::string guessSemantic(const std::string& paramName, const std::string& unit) {
  std::string n;
  for (unsigned char c : paramName) n += static_cast<char>(std::tolower(c));
  const std::string u = normalizeName(unit);
  if (has(n, "cutoff") || has(n, "lowpass") || has(n, "lpf") || (has(n, "filter") && (u == "hz" || u == "khz" || has(n, "freq")))) return "cutoff";
  if (has(n, "reso") || hasWord(n, "q")) return "resonance";
  if (has(n, "drive") || has(n, "distort") || has(n, "saturat") || has(n, "overdrive") || has(n, "fuzz")) return "drive";
  if (has(n, "attack")) return "attack";
  if (has(n, "decay")) return "decay";
  if (has(n, "sustain")) return "sustain";
  if (has(n, "release")) return "release";
  if (has(n, "dry/wet") || has(n, "drywet") || hasWord(n, "mix") || hasWord(n, "wet") || has(n, "blend")) return "mix";
  if (has(n, "threshold")) return "threshold";
  if (hasWord(n, "ratio")) return "ratio";
  if (has(n, "makeup")) return "makeup";
  if (has(n, "ceiling")) return "ceiling";
  if (has(n, "width") || has(n, "stereo")) return "width";
  if (hasWord(n, "pan") || has(n, "balance")) return "pan";
  if (has(n, "detune")) return "detune";
  if (has(n, "tune") || has(n, "pitch") || has(n, "coarse") || hasWord(n, "fine")) return "tune";
  if (has(n, "feedback")) return "feedback";
  if (hasWord(n, "time") || has(n, "delay") || has(n, "length") || has(n, "size") || has(n, "decay time")) return "time";
  if (hasWord(n, "rate") || has(n, "speed") || has(n, "lfo")) return "rate";
  if (hasWord(n, "depth") || has(n, "amount") || has(n, "intensity")) return "depth";
  if (has(n, "bass") || hasWord(n, "low") || has(n, "lows")) return "bass";
  if (has(n, "treble") || hasWord(n, "high") || hasWord(n, "air") || has(n, "bright") || hasWord(n, "tone")) return "treble";
  if (hasWord(n, "mid") || has(n, "presence")) return "himid";
  if (has(n, "gain") || has(n, "volume") || hasWord(n, "level") || has(n, "output") || hasWord(n, "master") || hasWord(n, "amp")) return "gain";
  return {};
}

bool tagMatches(const std::string& tag, const std::string& paramTag) {
  if (tag.empty() || paramTag.empty()) return false;
  if (tag[0] == '=') return tag.compare(1, std::string::npos, paramTag) == 0;   // "=attack": exactly that tag, not the compressor's
  return tag == paramTag || paramTag == "comp_" + tag;
}

namespace {
struct Phrase { std::string text; std::vector<Move> moves; };

// Longest phrases first; a matched phrase is blanked so shorter ones inside it do not match again.
const std::vector<Phrase>& phrases() {
  static const std::vector<Phrase> p = [] {
    std::vector<Phrase> v = {
      {"less harsh", {{"himid", -1}, {"treble", -1}}}, {"less piercing", {{"himid", -1}, {"treble", -1}}}, {"tame the highs", {{"treble", -1}, {"himid", -1}}},
      {"less bright", {{"treble", -1}, {"cutoff", -1}, {"himid", -1}}}, {"more air", {{"treble", 1}}}, {"opens up", {{"treble", 1}, {"cutoff", 1}}}, {"open up", {{"treble", 1}, {"cutoff", 1}}},
      {"brighter", {{"treble", 1}, {"cutoff", 1}, {"himid", 1}}}, {"brighten", {{"treble", 1}, {"cutoff", 1}, {"himid", 1}}}, {"more bright", {{"treble", 1}, {"cutoff", 1}, {"himid", 1}}},
      {"darker", {{"treble", -1}, {"cutoff", -1}, {"himid", -1}}}, {"darken", {{"treble", -1}, {"cutoff", -1}, {"himid", -1}}}, {"duller", {{"treble", -1}, {"cutoff", -1}}}, {"mellower", {{"treble", -1}, {"cutoff", -1}}},
      {"warmer", {{"bass", 1}, {"treble", -1}}}, {"warm it up", {{"bass", 1}, {"treble", -1}}}, {"colder", {{"treble", 1}, {"bass", -1}}}, {"cooler", {{"treble", 1}, {"bass", -1}}},
      {"smoother", {{"himid", -1}, {"treble", -1}}}, {"thicker", {{"bass", 1}, {"lowmid", 1}}}, {"fuller", {{"bass", 1}, {"lowmid", 1}}}, {"fatter", {{"bass", 1}, {"lowmid", 1}}}, {"more body", {{"bass", 1}, {"lowmid", 1}}},
      {"thinner", {{"bass", -1}, {"lowmid", -1}}}, {"less muddy", {{"lowmid", -1}, {"bass", -1}}}, {"muddy", {{"lowmid", -1}, {"bass", -1}}}, {"less boomy", {{"bass", -1}}}, {"boomy", {{"bass", -1}}},
      {"more low end", {{"bass", 1}}}, {"less low end", {{"bass", -1}}},
      {"punchier", {{"comp_attack", 1}, {"=attack", -1}}}, {"more punch", {{"comp_attack", 1}, {"=attack", -1}}},
      {"tighter", {{"release", -1}, {"decay", -1}, {"ratio", 1}}}, {"snappier", {{"release", -1}, {"decay", -1}, {"sustain", -1}}}, {"staccato", {{"release", -1}, {"decay", -1}, {"sustain", -1}}},
      {"more sustain", {{"release", 1}, {"decay", 1}, {"sustain", 1}}}, {"longer tail", {{"release", 1}, {"decay", 1}}}, {"ringier", {{"release", 1}, {"decay", 1}, {"sustain", 1}}},
      {"louder", {{"gain", 1}, {"makeup", 1}}}, {"quieter", {{"gain", -1}}}, {"softer", {{"gain", -1}}},
      {"wider", {{"width", 1}}}, {"more stereo", {{"width", 1}}}, {"spacious", {{"width", 1}}}, {"narrower", {{"width", -1}}}, {"more mono", {{"width", -1}}},
      {"dirtier", {{"drive", 1}}}, {"grittier", {{"drive", 1}}}, {"more distortion", {{"drive", 1}}}, {"cleaner", {{"drive", -1}}}, {"less distortion", {{"drive", -1}}},
      {"squelchy", {{"resonance", 1}}}, {"wetter", {{"mix", 1}}}, {"drier", {{"mix", -1}}}, {"more reverb", {{"mix", 1}}}, {"less reverb", {{"mix", -1}}}, {"more echo", {{"mix", 1}}}, {"less echo", {{"mix", -1}}},
      {"more compression", {{"threshold", -1}, {"ratio", 1}}}, {"heavier compression", {{"threshold", -1}, {"ratio", 1}}}, {"squash", {{"threshold", -1}, {"ratio", 1}}}, {"glue", {{"threshold", -1}, {"ratio", 1}}},
      {"less compression", {{"threshold", 1}, {"ratio", -1}}}, {"gentler", {{"threshold", 1}, {"ratio", -1}}}, {"more dynamic", {{"threshold", 1}, {"ratio", -1}}},
      {"pan left", {{"pan", -1}}}, {"pan right", {{"pan", 1}}}, {"to the left", {{"pan", -1}}}, {"to the right", {{"pan", 1}}},
    };
    // "more X", "less X" and a few synonyms for each known tag
    static const char* up[] = {"more", "increase", "raise", "boost", "higher", "longer", "bigger", "add"};
    static const char* down[] = {"less", "decrease", "lower", "reduce", "cut", "shorter", "smaller", "remove"};
    for (const std::string& t : knownTags()) {
      if (t == "himid" || t == "lowmid" || t == "frequency" || t == "q" || t == "level") continue;
      for (const char* w : up) v.push_back({std::string(w) + " " + t, {{t, 1}}});
      for (const char* w : down) v.push_back({std::string(w) + " " + t, {{t, -1}}});
    }
    std::stable_sort(v.begin(), v.end(), [](const Phrase& a, const Phrase& b) { return a.text.size() > b.text.size(); });
    return v;
  }();
  return p;
}

bool containsPhrase(const std::string& hay, const std::string& needle, std::size_t& at) {
  std::size_t pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    const bool left = pos == 0 || !std::isalpha(static_cast<unsigned char>(hay[pos - 1]));
    const std::size_t e = pos + needle.size();
    const bool right = e >= hay.size() || !std::isalpha(static_cast<unsigned char>(hay[e]));
    if (left && right) { at = pos; return true; }
    pos = e;
  }
  return false;
}
}  // namespace

std::vector<std::string> knownTags() {
  return {"cutoff", "resonance", "drive", "attack", "comp_attack", "comp_release", "decay", "sustain", "release", "mix", "threshold", "ratio", "makeup", "ceiling", "knee", "width", "pan", "detune", "tune", "feedback", "time", "rate", "depth", "bass", "lowmid", "himid", "treble", "gain", "frequency", "q", "level"};
}

Intent parseIntent(const std::string& text) {
  Intent in;
  std::string s;
  for (unsigned char c : text) s += static_cast<char>(std::tolower(c));
  // how much: modifiers change the step size
  struct Mod { const char* w; double a; };
  static const Mod mods[] = {{"a little", 0.07}, {"a bit", 0.07}, {"slightly", 0.07}, {"a touch", 0.05}, {"a tad", 0.05}, {"subtly", 0.05}, {"just a", 0.07},
                             {"a lot", 0.3}, {"much", 0.3}, {"way", 0.3}, {"really", 0.3}, {"very", 0.3}, {"heavily", 0.3}, {"significantly", 0.3}, {"all the way", 0.5}, {"extremely", 0.5}, {"max", 0.5}};
  for (const Mod& m : mods) { std::size_t at; if (containsPhrase(s, m.w, at)) { in.amount = m.a; for (std::size_t i = 0; i < std::string(m.w).size(); ++i) s[at + i] = ' '; break; } }
  // filler words do not change what is meant ("lower the cutoff" = "lower cutoff")
  {
    std::string out, word;
    auto flush = [&] {
      static const char* filler[] = {"the", "my", "its", "that", "this", "some", "of", "it", "to", "a", "an"};
      bool skip = false;
      for (const char* f : filler) if (word == f) skip = true;
      if (!word.empty() && !skip) out += (out.empty() ? "" : " ") + word;
      word.clear();
    };
    for (char c : s) { if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'') word += c; else flush(); }
    flush();
    s = out;
  }
  for (const Phrase& p : phrases()) {
    std::size_t at;
    while (containsPhrase(s, p.text, at)) {
      in.matched.push_back(p.text);
      for (const Move& m : p.moves) in.moves.push_back(m);
      for (std::size_t i = 0; i < p.text.size(); ++i) s[at + i] = ' ';
    }
  }
  return in;
}

}  // namespace wezt::agent
