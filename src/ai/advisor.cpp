#include "ai/advisor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace wezt::ai {
namespace {
template <class... A>
std::string fmt(const char* f, A... a) {
  char s[300];
  std::snprintf(s, sizeof s, f, a...);  // NOLINT: format strings are literals at every call site
  return s;
}
float* biasFor(Preferences& p, const Suggestion& s) {
  switch (s.kind) {
    case SuggestionKind::Loudness: return &p.loudnessBias;
    case SuggestionKind::EqBand: return s.band >= 0 && s.band < kBands ? &p.bandBias[static_cast<std::size_t>(s.band)] : nullptr;
    case SuggestionKind::Dynamics: return &p.crestBias;
    case SuggestionKind::Width: return &p.widthBias;
  }
  return nullptr;
}
}  // namespace

std::string Suggestion::key() const {
  switch (kind) {
    case SuggestionKind::Loudness: return "loudness";
    case SuggestionKind::EqBand: return "eq:" + std::to_string(band);
    case SuggestionKind::Dynamics: return "dynamics";
    case SuggestionKind::Width: return "width";
  }
  return "?";
}

float Preferences::acceptRate(const std::string& key) const {
  auto it = record.find(key);
  if (it == record.end()) return 0.5f;
  return (static_cast<float>(it->second.first) + 1.f) / (static_cast<float>(it->second.second) + 2.f);  // Laplace smoothing
}

std::string Preferences::serialize() const {
  std::ostringstream o;
  o.precision(9);
  o << "WEZT-PREFS 1\nbias " << loudnessBias << " " << crestBias << " " << widthBias;
  for (float b : bandBias) o << " " << b;
  o << "\n";
  for (auto& [k, v] : record) o << "rec " << k << " " << v.first << " " << v.second << "\n";
  o << "end\n";
  return o.str();
}
bool Preferences::parse(const std::string& text, Preferences& out) {
  std::istringstream is(text);
  std::string line;
  if (!std::getline(is, line) || line.rfind("WEZT-PREFS 1", 0) != 0) return false;
  Preferences p;
  bool ended = false;
  while (std::getline(is, line)) {
    std::istringstream ls(line);
    std::string k;
    ls >> k;
    if (k == "bias") {
      ls >> p.loudnessBias >> p.crestBias >> p.widthBias;
      for (auto& b : p.bandBias) ls >> b;
      if (!ls && !ls.eof()) return false;
    } else if (k == "rec") {
      std::string key; int a = 0, n = 0;
      ls >> key >> a >> n;
      if (!ls || a < 0 || n < a) return false;
      p.record[key] = {a, n};
    } else if (k == "end") { ended = true; break; }
  }
  auto ok = [](float v) { return std::isfinite(v) && std::fabs(v) < 100.f; };
  if (!ended || !ok(p.loudnessBias) || !ok(p.crestBias) || !ok(p.widthBias)) return false;
  for (float b : p.bandBias) if (!ok(b)) return false;
  out = std::move(p);
  return true;
}

std::vector<Suggestion> advise(const Features& f, const StyleProfile& target, const Preferences& pref, const AdviceOptions& o) {
  std::vector<Suggestion> r;
  if (!f.valid() || target.count() < 1) return r;
  const Features t = target.mean();
  const auto sd = target.stddev();
  // how many standard deviations the gap is; the floor keeps a one-track profile from exploding
  auto conf = [&](float gap, float floorSd, std::size_t idx, const std::string& key) {
    const float s = std::max(floorSd, idx < sd.size() ? sd[idx] : 0.f);
    const float z = std::fabs(gap) / s;
    return std::clamp(z / 3.f, 0.f, 1.f) * (0.5f + pref.acceptRate(key));
  };
  auto push = [&](Suggestion s) {
    s.confidence = std::clamp(s.confidence, 0.f, 1.f);
    if (s.confidence >= o.minConfidence) r.push_back(std::move(s));
  };

  {  // loudness
    const float gap = t.lufs - f.lufs + pref.loudnessBias;
    if (std::fabs(gap) >= o.minLoudnessLu) {
      Suggestion s; s.kind = SuggestionKind::Loudness; s.value = gap;
      s.confidence = conf(gap, 1.f, 0, s.key());
      s.text = fmt("Loudness is %.1f LUFS; the target is %.1f. %s by about %.1f LU.", f.lufs, t.lufs, gap > 0 ? "Raise it" : "Lower it", std::fabs(gap));
      push(s);
    }
  }
  for (int b = 0; b < kBands; ++b) {  // tonal balance
    const std::size_t bi = 9 + static_cast<std::size_t>(b);
    const float diff = t.bandDb[static_cast<std::size_t>(b)] - f.bandDb[static_cast<std::size_t>(b)];
    if (std::fabs(diff) < o.minEqDb) continue;
    Suggestion s; s.kind = SuggestionKind::EqBand; s.band = b;
    s.value = std::clamp(diff * o.eqStrength + pref.bandBias[static_cast<std::size_t>(b)], -o.maxEqDb, o.maxEqDb);
    if (std::fabs(s.value) < 0.5f) continue;
    s.confidence = conf(diff, 2.f, bi, s.key());
    s.text = fmt("Around %.0f Hz the track is %.1f dB %s than the target. Try %+.1f dB there.", kBandCenters[static_cast<std::size_t>(b)], std::fabs(diff), diff > 0 ? "weaker" : "stronger", s.value);
    push(s);
  }
  {  // dynamics
    const float gap = t.crestDb - f.crestDb + pref.crestBias;
    if (std::fabs(gap) >= o.minCrestDb) {
      Suggestion s; s.kind = SuggestionKind::Dynamics; s.value = gap;
      s.confidence = conf(gap, 1.5f, 4, s.key());
      s.text = fmt("Peak-to-RMS is %.1f dB; the target is %.1f. %s.", f.crestDb, t.crestDb, gap < 0 ? "Compress or limit a little more" : "Ease off the compression to keep more punch");
      push(s);
    }
  }
  {  // stereo width
    const float gap = t.stereoWidth - f.stereoWidth + pref.widthBias;
    if (std::fabs(gap) >= o.minWidth) {
      Suggestion s; s.kind = SuggestionKind::Width; s.value = gap;
      s.confidence = conf(gap, 0.05f, 6, s.key());
      s.text = fmt("Stereo width is %.0f%% of the target's (%.2f vs %.2f side share). %s.", 100.0 * f.stereoWidth / std::max(1e-3f, t.stereoWidth), f.stereoWidth, t.stereoWidth, gap > 0 ? "Widen the image" : "Narrow the image");
      push(s);
    }
  }
  std::sort(r.begin(), r.end(), [](const Suggestion& a, const Suggestion& b) { return a.confidence > b.confidence; });
  return r;
}

void learn(Preferences& p, const Suggestion& s, bool accepted, float applied, float rate) {
  auto& rec = p.record[s.key()];
  ++rec.second;
  float* bias = biasFor(p, s);
  if (accepted) {
    ++rec.first;
    if (bias) *bias += rate * (applied - s.value);  // they wanted a bit more or less than we said
  } else if (bias) {
    *bias -= rate * s.value;                        // they like it as it is: pull the target toward the current state
  }
  if (bias) *bias = std::clamp(*bias, -12.f, 12.f);
}

}  // namespace wezt::ai
