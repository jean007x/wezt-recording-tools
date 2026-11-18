#include "voice/voice_takes.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <optional>
#include <sstream>

#include "music/audio_to_midi.h"
#include "voice/voice_bank.h"

namespace wezt::voice {

bool measureFormants(const AudioBuffer& audio, double sampleRate, double& f1, double& f2);   // voice_analysis.cpp

namespace {
double centroidOf(const float* x, std::size_t n, double sr) {
  if (n < 1024) return 0;
  std::size_t N = 1024; double num = 0, den = 0; std::size_t frames = 0;
  for (std::size_t i = 0; i + N <= n; i += N / 2, ++frames) {
    // plain DFT on every other bin up to Nyquist (a few frames only, so speed does not matter)
    for (int k = 4; k < 512; k += 2) {
      double re = 0, im = 0; const double w0 = 2.0 * M_PI * k / static_cast<double>(N);
      for (std::size_t j = 0; j < N; ++j) { const double win = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(j) / static_cast<double>(N)); re += x[i + j] * win * std::cos(w0 * static_cast<double>(j)); im -= x[i + j] * win * std::sin(w0 * static_cast<double>(j)); }
      const double p = re * re + im * im; num += p * k * sr / static_cast<double>(N); den += p;
    }
    if (frames >= 6) break;
  }
  return den > 0 ? num / den : 0;
}
}  // namespace

std::vector<Take> splitTakes(const AudioBuffer& audio, double sr, double minPause, double minTake) {
  std::vector<Take> out;
  if (audio.frames < static_cast<std::size_t>(sr * 0.2) || audio.channels < 1) return out;
  const float* x = audio.channel(0);
  const std::size_t hop = static_cast<std::size_t>(sr * 0.01);
  const std::size_t n = audio.frames / hop;
  std::vector<double> db(n);
  double peak = -200;
  for (std::size_t i = 0; i < n; ++i) { double e = 0; for (std::size_t j = 0; j < hop; ++j) e += double(x[i * hop + j]) * x[i * hop + j]; db[i] = 10.0 * std::log10(e / static_cast<double>(hop) + 1e-12); peak = std::max(peak, db[i]); }
  const double thr = std::max(-50.0, peak - 32.0);
  const std::size_t gapFrames = static_cast<std::size_t>(minPause / 0.01);
  std::vector<std::pair<std::size_t, std::size_t>> segs; bool in = false; std::size_t s = 0, gap = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (db[i] > thr) { if (!in) { in = true; s = i; } gap = 0; }
    else if (in && ++gap > gapFrames) { segs.push_back({s, i - gap + 1}); in = false; }
  }
  if (in) segs.push_back({s, n});
  const auto pitch = music::trackPitch(audio, sr);
  for (auto [a, b] : segs) {
    const double t0 = static_cast<double>(a) * 0.01, t1 = static_cast<double>(b) * 0.01;
    if (t1 - t0 < minTake) continue;
    Take t; t.from = t0; t.to = t1;
    std::vector<double> hz; int total = 0, voiced = 0;
    for (const auto& f : pitch) if (f.time >= t0 && f.time < t1) { ++total; if (f.hz > 0 && f.confidence > 0.6) { ++voiced; hz.push_back(f.hz); if (t.voicedFrom < 0) t.voicedFrom = f.time - t0; } }
    t.voicedShare = total ? static_cast<double>(voiced) / total : 0;
    if (!hz.empty()) { std::sort(hz.begin(), hz.end()); t.f0 = hz[hz.size() / 2]; }
    const std::size_t s0 = static_cast<std::size_t>(t0 * sr), s1 = std::min(audio.frames, static_cast<std::size_t>(t1 * sr));
    const double dur = t1 - t0;
    if (t.voicedShare < 0.12) {
      t.kind = dur >= 0.15 ? "hiss" : "unclear";
      t.hissCentroidHz = centroidOf(x + s0 + (s1 - s0) / 4, (s1 - s0) / 2, sr);
      if (t.kind == "hiss") t.guess = t.hissCentroidHz > 4300 ? "s" : "sh";
    } else {
      const std::size_t v0 = s0 + static_cast<std::size_t>(std::max(0.0, t.voicedFrom) * sr);
      if (t.voicedFrom >= 0.08) { t.kind = "syllable"; t.hissCentroidHz = centroidOf(x + s0, v0 - s0, sr); }
      else t.kind = "vowel";
      if (s1 > v0 + static_cast<std::size_t>(0.3 * sr)) {
        AudioBuffer seg(1, s1 - v0); std::copy(x + v0, x + s1, seg.channel(0));
        double f1 = 0, f2 = 0;
        if (measureFormants(seg, sr, f1, f2)) { t.f1 = f1; t.f2 = f2; if (t.kind == "vowel") t.guess = nearestVowel(f1, f2); }
      } else if (t.kind == "vowel") t.kind = "voiced-short";
    }
    out.push_back(t);
  }
  return out;
}


namespace {
#if defined(__GNUC__) || defined(__clang__)
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
#endif
std::string fmt(const char* f, ...) { char b[512]; va_list ap; va_start(ap, f); std::vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }
AudioBuffer sliceTake(const AudioBuffer& a, double sr, double t0, double t1) {
  const std::size_t s = static_cast<std::size_t>(std::max(0.0, t0 - 0.05) * sr), z = std::min(a.frames, static_cast<std::size_t>((t1 + 0.05) * sr));
  AudioBuffer o(1, z > s ? z - s : 0); if (z > s) std::copy(a.channel(0) + s, a.channel(0) + z, o.channel(0)); return o;
}
}  // namespace

BuildReport buildBankFromTakes(const AudioBuffer& mono, double sr, const std::vector<Take>& takes, const std::string& use, VoiceBank& bank, bool derive, double minSteady) {
  BuildReport rep; int ok = 0, bad = 0;
  auto say = [&](const char* f, auto... args) { rep.log += fmt(f, args...); };
  std::stringstream ss(use); std::string item; 
  while (std::getline(ss, item, ',')) {
    const auto eq = item.find('='); if (eq == std::string::npos) continue;
    const std::size_t idx = static_cast<std::size_t>(std::atoi(item.substr(0, eq).c_str())); std::string name = item.substr(eq + 1); bool trailing = false; if (name.size() > 5 && name.compare(name.size() - 5, 5, "@tail") == 0) { trailing = true; name.resize(name.size() - 5); }
    if (idx >= takes.size()) { say("take %zu does not exist\n", idx); ++bad; continue; }
    const auto& t = takes[idx]; const AudioBuffer seg = sliceTake(mono, sr, t.from, t.to);
    if (trailing) {   // "name@tail": the consonant comes after the vowel ("ef", "en", "em", "el", "ess")
      auto cr = makeTrailingConsonantEntry(name, seg, sr); say("take %zu -> %s\n", idx, cr.message.c_str());
      if (cr.ok) { bank.consonants.erase(std::remove_if(bank.consonants.begin(), bank.consonants.end(), [&](const ConsonantEntry& c) { return c.name == name; }), bank.consonants.end()); bank.consonants.push_back(cr.entry); ++ok; } else ++bad;
    } else if (name == "p" || name == "t" || name == "k" || name == "b" || name == "d" || name == "g" || name == "j" || name == "z") {   // quick stops: keep only the burst, whatever the take looks like
      auto cr = makeShortConsonantEntry(name, seg, sr); say("take %zu -> %s\n", idx, cr.message.c_str());
      if (cr.ok) { bank.consonants.erase(std::remove_if(bank.consonants.begin(), bank.consonants.end(), [&](const ConsonantEntry& c) { return c.name == name; }), bank.consonants.end()); bank.consonants.push_back(cr.entry); ++ok; } else ++bad;
    } else if (t.kind == "hiss") {
      if (!isConsonantName(name)) { say("take %zu is a hiss; \"%s\" is not a consonant\n", idx, name.c_str()); ++bad; continue; }
      bool have = false; for (const auto& c : bank.consonants) have = have || c.name == name;
      if (have) { say("take %zu: \"%s\" is already in the bank, skipped\n", idx, name.c_str()); continue; }
      const std::size_t n = seg.frames, s0 = n * 15 / 100, s1 = n * 65 / 100; ConsonantEntry ce; ce.name = name; ce.rate = sr; ce.samples.assign(seg.channel(0) + s0, seg.channel(0) + s1);
      const std::size_t F = static_cast<std::size_t>(0.02 * sr); for (std::size_t i = 0; i < F && i < ce.samples.size() / 2; ++i) { const float g = static_cast<float>(i) / F; ce.samples[i] *= g; ce.samples[ce.samples.size() - 1 - i] *= g; }
      double e = 0; for (float v : ce.samples) e += double(v) * v; const double rms = std::sqrt(e / std::max<std::size_t>(1, ce.samples.size()));
      static const std::map<std::string, double> level = {{"s", 0.2}, {"sh", 0.35}, {"f", 0.15}, {"h", 0.2}, {"th", 0.1}};
      const double rel = level.count(name) ? level.at(name) : 0.25;
      if (rms > 0) for (auto& v : ce.samples) v = static_cast<float>(v * kBankRms * rel / rms);
      bank.consonants.push_back(ce); say("take %zu -> \"%s\": hiss kept, %.0f ms, set to a typical level\n", idx, name.c_str(), ce.samples.size() * 1000.0 / sr); ++ok;
    } else if (isConsonantName(name)) {
      auto cr = makeConsonantEntry(name, seg, sr); say("take %zu -> %s\n", idx, cr.message.c_str());
      if (cr.ok) {
        // Takes are often spoken louder than a consonant sits next to its vowel in singing: keep the sound under these ceilings.
        static const std::map<std::string, double> ceiling = {{"p", .4}, {"t", .4}, {"k", .4}, {"f", .4}, {"s", .4}, {"sh", .4}, {"ch", .4}, {"th", .3}, {"h", .3}, {"b", .6}, {"d", .6}, {"g", .6}, {"v", .6}, {"z", .6}, {"j", .6}};
        const auto cap = ceiling.find(name);
        if (cap != ceiling.end()) { double e = 0; for (float v : cr.entry.samples) e += double(v) * v; const double rms = std::sqrt(e / std::max<std::size_t>(1, cr.entry.samples.size())); if (rms > kBankRms * cap->second) { const float g = static_cast<float>(kBankRms * cap->second / rms); for (auto& v : cr.entry.samples) v *= g; say("      turned down to %.0f dB under the vowel\n", -20.0 * std::log10(cap->second)); } }
        bank.consonants.push_back(cr.entry); ++ok;
      } else ++bad;
    } else if (name != "ah" && name != "ee" && name != "eh" && name != "ae" && name != "ih" && name != "oh" && name != "oo" && name != "uh" && name != "er") {
      say("take %zu: \"%s\" is not a vowel or consonant I know\n", idx, name.c_str()); ++bad;
    } else {
      const auto vr = makeBankEntry(name, seg, sr, minSteady); say("take %zu -> %s\n", idx, vr.message.c_str()); if (vr.ok) { bank.entries.push_back(vr.entry); ++ok; } else ++bad;
    }
  }

  if (derive) {   // fill the consonants nobody recorded from the person's own sounds that are already in the bank
    auto find = [&](const std::string& n) -> const ConsonantEntry* { for (const auto& c : bank.consonants) if (c.name == n) return &c; return nullptr; };
    auto vowelOf = [&](const std::string& v) -> const BankEntry* { for (const auto& e : bank.entries) if (e.vowel == v) return &e; return nullptr; };
    auto rmsOf = [](const std::vector<float>& v) { double e = 0; for (float x : v) e += double(x) * x; return std::sqrt(e / std::max<std::size_t>(1, v.size())); };
    auto fit = [&](std::vector<float>& v, double rms, double sec, double sr0) {   // trim to sec, fade, scale
      v.resize(std::min(v.size(), static_cast<std::size_t>(sec * sr0)));
      const std::size_t fi = std::min<std::size_t>(static_cast<std::size_t>(0.012 * sr0), v.size() / 4), fo = std::min<std::size_t>(static_cast<std::size_t>(0.03 * sr0), v.size() / 3);
      for (std::size_t i = 0; i < fi; ++i) v[i] *= static_cast<float>(i) / fi;
      for (std::size_t i = 0; i < fo; ++i) v[v.size() - 1 - i] *= static_cast<float>(i) / fo;
      const double r0 = rmsOf(v); if (r0 > 0) for (auto& x : v) x = static_cast<float>(x * rms / r0); };
    auto addC = [&](const std::string& n, std::vector<float> smp, double rate, const char* how) { ConsonantEntry c; c.name = n; c.rate = rate; c.samples = std::move(smp); bank.consonants.push_back(c); say("derived \"%s\": %s\n", n.c_str(), how); };
    for (auto [cons, vow] : {std::pair<const char*, const char*>{"w", "oo"}, {"y", "ee"}, {"r", "er"}}) {   // glides: the start of the person's own vowel
      if (find(cons)) continue;
      const BankEntry* e = vowelOf(vow);
      if (!e) continue;
      std::vector<float> v(e->samples.begin(), e->samples.end()); fit(v, kBankRms * 0.6, 0.09, e->rate); addC(cons, v, e->rate, (std::string("the first 90 ms of your \"") + vow + "\"").c_str());
    }
    // copies, not pointers: adding a derived consonant can reallocate bank.consonants and would leave pointers dangling
    auto copyOf = [&](const char* nm) -> std::optional<ConsonantEntry> { const ConsonantEntry* c = find(nm); return c ? std::optional<ConsonantEntry>(*c) : std::nullopt; };
    const auto t = copyOf("t"), sh = copyOf("sh"), p = copyOf("p"), m = copyOf("m"), f = copyOf("f"), n = copyOf("n");
    if (!find("ch") && t && sh) { std::vector<float> v = t->samples; v.insert(v.end(), sh->samples.begin(), sh->samples.begin() + std::min<std::size_t>(sh->samples.size(), static_cast<std::size_t>(0.16 * sh->rate))); addC("ch", v, t->rate, "your t burst followed by your sh"); }
    if (!find("b") && p && m) { std::vector<float> v(m->samples.begin(), m->samples.begin() + std::min<std::size_t>(m->samples.size(), static_cast<std::size_t>(0.04 * m->rate))); for (auto& x : v) x *= 0.5f; v.insert(v.end(), p->samples.begin(), p->samples.end()); addC("b", v, p->rate, "40 ms of your m murmur, then your p burst"); }
    if (!find("v") && f && n) { const std::size_t L = std::min({f->samples.size(), n->samples.size(), static_cast<std::size_t>(0.14 * f->rate)}); std::vector<float> v(L); for (std::size_t i = 0; i < L; ++i) v[i] = 0.5f * f->samples[i] + 0.5f * n->samples[i]; addC("v", v, f->rate, "your f hiss mixed with your n murmur"); }
    if (!find("th") && f) { std::vector<float> v = f->samples; for (auto& x : v) x *= 0.6f; addC("th", v, f->rate, "your f, 4 dB quieter"); }
    if (!find("ng") && n) addC("ng", n->samples, n->rate, "your n");
    if (!find("h") && sh) { std::vector<float> v = sh->samples; float lp = 0; for (auto& x : v) { lp += 0.25f * (x - lp); x = lp; } fit(v, kBankRms * 0.12, 0.12, sh->rate); addC("h", v, sh->rate, "your sh, softened into a breath");}
  }
  rep.used = ok; rep.skipped = bad;
  return rep;
}

}  // namespace wezt::voice
