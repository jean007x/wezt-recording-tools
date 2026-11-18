#include "ai/mix_assistant.h"

#include <algorithm>
#include <cmath>

namespace wezt::ai {
namespace {

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

struct Spec { std::vector<float> abs; float rms = -100.f; };

Spec measure(const AudioBuffer& a, double sr) {
  Features f = analyze(a, sr);
  Spec s;
  s.rms = f.rmsDb;
  s.abs.resize(kBands);
  for (int b = 0; b < kBands; ++b) s.abs[static_cast<std::size_t>(b)] = f.bandDb[static_cast<std::size_t>(b)] + f.rmsDb;
  return s;
}

}  // namespace

Role guessRole(const std::string& name0) {
  const std::string n = lower(name0);
  if (has(n, "kick") || has(n, "bd ")) return Role::Kick;
  if (has(n, "vox") || has(n, "vocal") || has(n, "voice") || has(n, "sing")) return Role::Vocal;
  if (has(n, "bass") || has(n, "808") || has(n, "sub")) return Role::Bass;
  if (has(n, "drum") || has(n, "snare") || has(n, "hat") || has(n, "perc") || has(n, "kit")) return Role::Drums;
  if (has(n, "lead") || has(n, "melody") || has(n, "solo")) return Role::Lead;
  if (has(n, "pad") || has(n, "string") || has(n, "ambient") || has(n, "atmos")) return Role::Pad;
  if (has(n, "guitar") || has(n, "gtr")) return Role::Guitar;
  if (has(n, "key") || has(n, "piano") || has(n, "rhodes") || has(n, "organ") || has(n, "synth")) return Role::Keys;
  return Role::Other;
}
int rolePriority(Role r) {
  switch (r) {
    case Role::Vocal: return 9; case Role::Lead: return 8; case Role::Kick: return 7; case Role::Bass: return 7;
    case Role::Drums: return 6; case Role::Guitar: return 4; case Role::Keys: return 3; case Role::Pad: return 2; case Role::Other: return 1;
  }
  return 1;
}

MixReport analyzeMix(const std::vector<MixTrack>& tracks, double sr) {
  MixReport rep;
  std::vector<Spec> specs;
  std::vector<Role> roles;
  for (auto& t : tracks) {
    specs.push_back(t.audio && t.audio->frames > 4096 ? measure(*t.audio, sr) : Spec());
    roles.push_back(guessRole(t.name));
  }
  // 1. Clashes: two tracks that are both strong in the same band, within 4 dB of each other.
  for (std::size_t i = 0; i < tracks.size(); ++i)
    for (std::size_t j = i + 1; j < tracks.size(); ++j) {
      if (specs[i].abs.empty() || specs[j].abs.empty()) continue;
      for (int b = 2; b < kBands - 1; ++b) {    // 125 Hz .. 8 kHz; the extremes are handled below
        const float a = specs[i].abs[static_cast<std::size_t>(b)], c = specs[j].abs[static_cast<std::size_t>(b)];
        const float peakA = *std::max_element(specs[i].abs.begin(), specs[i].abs.end());
        const float peakC = *std::max_element(specs[j].abs.begin(), specs[j].abs.end());
        const bool strongA = a >= peakA - 6.f, strongC = c >= peakC - 6.f;    // a main region of each track
        if (!strongA || !strongC || std::fabs(a - c) > 4.f) continue;
        if (std::min(a, c) < -60.f) continue;
        const bool iWins = rolePriority(roles[i]) >= rolePriority(roles[j]);
        Clash cl;
        cl.winner = tracks[iWins ? i : j].name; cl.loser = tracks[iWins ? j : i].name;
        cl.band = b; cl.winnerDb = iWins ? a : c; cl.loserDb = iWins ? c : a;
        rep.clashes.push_back(cl);
      }
    }
  // 2. One fix per (loser, band): a narrow cut, deeper the closer the levels were.
  for (auto& c : rep.clashes) {
    bool dup = false;
    for (auto& f : rep.fixes) if (f.track == c.loser && std::fabs(f.eq.freq - kBandCenters[static_cast<std::size_t>(c.band)]) < 1.f) dup = true;
    if (dup) continue;
    MixFix f;
    f.track = c.loser;
    f.eq.type = dsp::FilterType::Peaking;
    f.eq.freq = kBandCenters[static_cast<std::size_t>(c.band)];
    f.eq.q = 1.2f;
    const float gap = c.winnerDb - c.loserDb;      // the louder winner needs less help
    f.eq.gainDb = -std::clamp(3.5f - 0.4f * gap, 1.5f, 4.f);
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s and %s both peak around %s (%.0f dB vs %.0f dB); cut %.1f dB on %s to leave room for %s",
                  c.winner.c_str(), c.loser.c_str(), kBandNames[c.band], c.winnerDb, c.loserDb, -f.eq.gainDb, c.loser.c_str(), c.winner.c_str());
    f.why = buf;
    rep.fixes.push_back(f);
  }
  // 3. Low-end discipline: only kick and bass need energy under 60 Hz. Others get a high-pass.
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (specs[i].abs.empty()) continue;
    if (roles[i] == Role::Kick || roles[i] == Role::Bass) continue;
    const float low = std::max(specs[i].abs[0], specs[i].abs[1]);
    const float peak = *std::max_element(specs[i].abs.begin(), specs[i].abs.end());
    if (low > peak - 14.f && low > -55.f) {
      MixFix f;
      f.track = tracks[i].name;
      f.eq.type = dsp::FilterType::HighPass;
      f.eq.freq = roles[i] == Role::Drums ? 60.f : 90.f;
      f.eq.q = 0.7f; f.eq.gainDb = 0.f;
      char buf[160];
      std::snprintf(buf, sizeof buf, "%s has low-end energy only the kick and bass should carry; high-pass at %.0f Hz", tracks[i].name.c_str(), f.eq.freq);
      f.why = buf;
      rep.fixes.push_back(f);
    }
  }
  // 4. Build-up: the sum of everything in 125-250 Hz compared with 1-2 kHz.
  double mud = 0, mid = 0;
  for (auto& s : specs) if (!s.abs.empty()) { mud += std::pow(10.0, (s.abs[2] + 0) / 10.0) + std::pow(10.0, s.abs[3] / 10.0); mid += std::pow(10.0, s.abs[5] / 10.0) + std::pow(10.0, s.abs[6] / 10.0); }
  if (mid > 0 && 10 * std::log10(mud / mid) > 6.0) rep.notes.push_back("The mix is heavy at 125-250 Hz compared with the midrange (" + std::to_string(static_cast<int>(10 * std::log10(mud / mid))) + " dB). Muddiness usually comes from several tracks stacking there.");
  if (rep.clashes.empty() && rep.fixes.empty()) rep.notes.push_back("No clear frequency clashes between these tracks.");
  return rep;
}

AudioBuffer applyFix(const AudioBuffer& audio, double sr, const MixFix& fix) {
  AudioBuffer out(audio.channels, audio.frames);
  dsp::ParametricEq eq;
  eq.setBands({fix.eq});
  eq.prepare(sr, 4096);
  const std::size_t block = 4096;
  for (std::size_t pos = 0; pos < audio.frames; pos += block) {
    const std::size_t n = std::min(block, audio.frames - pos);
    AudioBuffer in(audio.channels, n), o(audio.channels, n);
    for (std::size_t c = 0; c < audio.channels; ++c) std::copy(audio.channel(c) + pos, audio.channel(c) + pos + n, in.channel(c));
    eq.process(in, o);
    for (std::size_t c = 0; c < audio.channels; ++c) std::copy(o.channel(c), o.channel(c) + n, out.channel(c) + pos);
  }
  return out;
}

AudioBuffer sidechainCompress(const AudioBuffer& target, const AudioBuffer& key, double sr, const SidechainParams& p) {
  AudioBuffer out(target.channels, target.frames);
  const float aA = std::exp(-1.f / (std::max(0.1f, p.attackMs) * 0.001f * static_cast<float>(sr)));
  const float aR = std::exp(-1.f / (std::max(1.f, p.releaseMs) * 0.001f * static_cast<float>(sr)));
  float env = 0.f;   // detector level, linear
  float gr = 0.f;    // current reduction, dB (positive)
  const float ratio = std::max(1.f, p.ratio);
  for (std::size_t i = 0; i < target.frames; ++i) {
    float k = 0.f;
    if (i < key.frames) for (std::size_t c = 0; c < key.channels; ++c) k = std::max(k, std::fabs(key.channel(c)[i]));
    env = k > env ? aA * env + (1.f - aA) * k : aR * env + (1.f - aR) * k;
    const float lv = env <= 1e-9f ? -180.f : 20.f * std::log10(env);
    const float over = lv - p.thresholdDb;
    const float want = over > 0.f ? std::min(p.maxReductionDb, over * (1.f - 1.f / ratio)) : 0.f;
    gr = want > gr ? want : aR * gr + (1.f - aR) * want;   // smooth the release
    const float g = std::pow(10.f, -gr / 20.f);
    for (std::size_t c = 0; c < target.channels; ++c) out.channel(c)[i] = target.channel(c)[i] * g;
  }
  return out;
}

}  // namespace wezt::ai
