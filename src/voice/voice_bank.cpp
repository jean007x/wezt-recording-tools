#include "voice/voice_bank.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

#include "voice/voice_model.h"

namespace wezt::voice {
bool measureFormants(const AudioBuffer& audio, double sampleRate, double& f1, double& f2);   // voice_analysis.cpp (its header clashes with voice_model.h)

const BankEntry* VoiceBank::find(const std::string& vowel, double f0) const {
  auto pick = [&](const std::string& v) -> const BankEntry* {
    const BankEntry* best = nullptr; double bd = 1e9;
    for (const auto& e : entries) if (e.vowel == v) {
      const double d = f0 > 0 && e.f0 > 0 ? std::fabs(std::log2(f0 / e.f0)) : 0.0;
      if (d < bd) { bd = d; best = &e; }
    }
    return best;
  };
  if (auto* e = pick(vowel)) return e;
  static const char* fb[][2] = {{"ae", "eh"}, {"ih", "ee"}, {"uh", "ah"}, {"er", "eh"}, {"eh", "ae"}, {"ee", "ih"}, {"ah", "uh"}, {"oh", "oo"}, {"oo", "oh"}};
  for (auto& p : fb) if (vowel == p[0]) if (auto* e = pick(p[1])) return e;
  return nullptr;
}

const ConsonantEntry* VoiceBank::findConsonant(const std::string& c) const {
  std::string n = c;
  if (n == "c" || n == "q") n = "k"; else if (n == "x") n = "s";
  for (const auto& e : consonants) if (e.name == n) return &e;
  return nullptr;
}

bool isConsonantName(const std::string& n) {
  static const char* names[] = {"p", "t", "k", "b", "d", "g", "f", "v", "s", "z", "sh", "ch", "th", "h", "m", "n", "l", "r", "w", "y", "j", "ng"};
  for (const char* x : names) if (n == x) return true;
  return false;
}


// A short stop consonant (p, t, k) in a take that has almost no held vowel after it, like a quick "tah" or "kah".
// The burst and its breath end where the vowel begins: the first frame whose energy sits mostly under 1.5 kHz
// and is still loud. Only that part is kept, scaled to the loudness it had next to its own vowel.
ConsonantResult makeShortConsonantEntry(const std::string& name, const AudioBuffer& audio, double sr) {
  ConsonantResult r; const std::string what = "\"" + name + "\"";
  const bool voicedStop = name == "b" || name == "d" || name == "g" || name == "j" || name == "z";
  if (name != "p" && name != "t" && name != "k" && !voicedStop) { r.message = what + " is not a short stop (p, t, k, b, d, g, j or z)."; return r; }
  const std::size_t win = static_cast<std::size_t>(sr * 0.010), hop = static_cast<std::size_t>(sr * 0.0025);
  if (audio.frames < win * 8) { r.message = "The recording for " + what + " is too short."; return r; }
  const float* x = audio.channel(0);
  struct F { double db, lowShare; }; std::vector<F> fr;
  for (std::size_t i = 0; i + win <= audio.frames; i += hop) {
    double e = 0; for (std::size_t k = 0; k < win; ++k) e += double(x[i + k]) * x[i + k];
    double low = 0, all = 0;   // share of the energy below 1.5 kHz: a vowel keeps most of it there, a burst or hiss does not
    for (int b = 2; b < 160; ++b) {   // bins up to about 7 kHz with a 10 ms window (100 Hz spacing)
      double re = 0, im = 0; const double w0 = 2.0 * M_PI * b / static_cast<double>(win);
      for (std::size_t k = 0; k < win; ++k) { const double wv = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(k) / static_cast<double>(win)); re += x[i + k] * wv * std::cos(w0 * static_cast<double>(k)); im -= x[i + k] * wv * std::sin(w0 * static_cast<double>(k)); }
      const double pw2 = re * re + im * im; all += pw2; if (b * sr / static_cast<double>(win) < 1500.0) low += pw2;
    }
    const double per = all > 0 ? low / all : 0;
    fr.push_back({10.0 * std::log10(e / static_cast<double>(win) + 1e-12), per});
  }
  double pk = -200; for (const auto& f : fr) pk = std::max(pk, f.db);
  if (pk < -70) { r.message = "The recording for " + what + " is silent."; return r; }
  std::size_t on = 0; while (on < fr.size() && fr[on].db < pk - 32) ++on;
  std::size_t vs = on + 2; while (vs < fr.size() && !(fr[vs].lowShare > 0.8 && fr[vs].db > pk - 20)) ++vs;
  if (vs >= fr.size()) { r.message = "I could not find the vowel after " + what + "."; return r; }
  const double t0 = static_cast<double>(on * hop) / sr, t1 = static_cast<double>(vs * hop) / sr;
  const double len = std::min(t1 - t0, 0.2);
  if (len < (voicedStop ? 0.008 : 0.012)) { r.message = "Almost nothing before the vowel for " + what + "."; return r; }
  const std::size_t s0 = static_cast<std::size_t>(t0 * sr), s1 = std::min(audio.frames, s0 + static_cast<std::size_t>(len * sr));
  ConsonantEntry c; c.name = name; c.rate = sr; c.samples.assign(x + s0, x + s1);
  double mean = 0; for (float v : c.samples) mean += v; mean /= static_cast<double>(c.samples.size());
  double cs = 0; for (float& v : c.samples) { v -= static_cast<float>(mean); cs += double(v) * v; }
  const double crms = std::sqrt(cs / static_cast<double>(c.samples.size()));
  const std::size_t v0 = static_cast<std::size_t>((t1 + 0.03) * sr), v1 = std::min(audio.frames, v0 + static_cast<std::size_t>(0.1 * sr));
  if (v1 <= v0 + 100) { r.message = "Not enough vowel after " + what + " to set its loudness."; return r; }
  double vs2 = 0; for (std::size_t i = v0; i < v1; ++i) vs2 += double(x[i]) * x[i];
  const double vrms = std::sqrt(vs2 / static_cast<double>(v1 - v0));
  if (crms <= 0 || vrms <= 0) { r.message = "Could not measure the loudness for " + what + "."; return r; }
  const double want = std::min(kBankRms * crms / vrms, kBankRms * (voicedStop ? 0.6 : 0.5));
  for (float& v : c.samples) v = static_cast<float>(v * want / crms);
  const std::size_t fi = std::min<std::size_t>(static_cast<std::size_t>(sr * 0.002), c.samples.size() / 4), fo = std::min<std::size_t>(static_cast<std::size_t>(sr * 0.006), c.samples.size() / 3);
  for (std::size_t i = 0; i < fi; ++i) c.samples[i] *= static_cast<float>(i) / fi;
  for (std::size_t i = 0; i < fo; ++i) c.samples[c.samples.size() - 1 - i] *= static_cast<float>(i) / fo;
  r.ok = true; r.entry = std::move(c);
  r.message = what + ": " + std::to_string(static_cast<int>(len * 1000)) + " ms burst, " + std::to_string(static_cast<int>(20.0 * std::log10(crms / vrms))) + " dB relative to the vowel (stored at most 6 dB under it).";
  return r;
}


// A consonant said AFTER its vowel, as in the letter names "ef", "en", "em", "el", "ess": the vowel, then the consonant.
// Hiss sounds (f, s, sh, th) are the part after the vowel's energy leaves the low frequencies; nasal and l sounds
// (n, m, l, ng) are the part after the vowel's level falls by 6 dB or more. Only the consonant is kept.
ConsonantResult makeTrailingConsonantEntry(const std::string& name, const AudioBuffer& audio, double sr) {
  ConsonantResult r; const std::string what = "\"" + name + "\"";
  const bool hiss = name == "f" || name == "s" || name == "sh" || name == "th";
  const bool sonorant = name == "n" || name == "m" || name == "l" || name == "ng";
  if (!hiss && !sonorant) { r.message = what + " cannot be cut from after a vowel (use f, s, sh, th, n, m, l or ng)."; return r; }
  const std::size_t win = static_cast<std::size_t>(sr * 0.010), hop = static_cast<std::size_t>(sr * 0.005);
  if (audio.frames < win * 20) { r.message = "The recording for " + what + " is too short."; return r; }
  const float* x = audio.channel(0);
  struct F { double db, lowShare; }; std::vector<F> fr;
  for (std::size_t i = 0; i + win <= audio.frames; i += hop) {
    double e = 0; for (std::size_t k = 0; k < win; ++k) e += double(x[i + k]) * x[i + k];
    double low = 0, all = 0;
    for (int b = 2; b < 160; ++b) {
      double re = 0, im = 0; const double w0 = 2.0 * M_PI * b / static_cast<double>(win);
      for (std::size_t k = 0; k < win; ++k) { const double wv = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(k) / static_cast<double>(win)); re += x[i + k] * wv * std::cos(w0 * static_cast<double>(k)); im -= x[i + k] * wv * std::sin(w0 * static_cast<double>(k)); }
      const double pw = re * re + im * im; all += pw; if (b * sr / static_cast<double>(win) < 1500.0) low += pw;
    }
    fr.push_back({10.0 * std::log10(e / static_cast<double>(win) + 1e-12), all > 0 ? low / all : 0});
  }
  double pk = -200; for (const auto& f : fr) pk = std::max(pk, f.db);
  if (pk < -70) { r.message = "The recording for " + what + " is silent."; return r; }
  std::size_t last = fr.size(); while (last > 0 && fr[last - 1].db < pk - 40) --last;   // end of the sound
  // the vowel: the loudest frames; its level is the median of the frames within 6 dB of the peak
  std::vector<double> top; for (const auto& f : fr) if (f.db > pk - 6) top.push_back(f.db);
  std::sort(top.begin(), top.end()); const double vowelDb = top[top.size() / 2];
  std::size_t lastVowel = 0; for (std::size_t i = 0; i < last; ++i) if (fr[i].db > pk - 8 && fr[i].lowShare > 0.75) lastVowel = i;
  std::size_t tail = lastVowel + 1;
  if (sonorant) { while (tail < last && fr[tail].db > vowelDb - 6) ++tail; }   // the level has to fall by 6 dB first
  if (last <= tail + 6) { r.message = "I could not find a " + what + " after the vowel."; return r; }
  std::size_t a = tail, z = last; if (z - a > 50) z = a + 50;
  const std::size_t s0 = a * hop + win / 2, s1 = std::min(audio.frames, z * hop + win / 2);
  if (s1 <= s0 + static_cast<std::size_t>(sr * 0.04)) { r.message = "The " + what + " after the vowel is too short."; return r; }
  ConsonantEntry c; c.name = name; c.rate = sr; c.samples.assign(x + s0, x + s1);
  double mean = 0; for (float v : c.samples) mean += v; mean /= static_cast<double>(c.samples.size());
  double cs = 0; for (float& v : c.samples) { v -= static_cast<float>(mean); cs += double(v) * v; }
  const double crms = std::sqrt(cs / static_cast<double>(c.samples.size()));
  const std::size_t v1 = std::min(audio.frames, lastVowel * hop), v0 = v1 > static_cast<std::size_t>(sr * 0.1) ? v1 - static_cast<std::size_t>(sr * 0.1) : 0;
  double vs2 = 0; for (std::size_t i = v0; i < v1; ++i) vs2 += double(x[i]) * x[i];
  const double vrms = std::sqrt(vs2 / static_cast<double>(std::max<std::size_t>(1, v1 - v0)));
  if (crms <= 0 || vrms <= 0) { r.message = "Could not measure the loudness for " + what + "."; return r; }
  const double cap = hiss ? 0.4 : 0.7;
  const double want = std::min(kBankRms * crms / vrms, kBankRms * cap);
  for (float& v : c.samples) v = static_cast<float>(v * want / crms);
  const std::size_t f = std::min<std::size_t>(static_cast<std::size_t>(sr * 0.01), c.samples.size() / 4);
  for (std::size_t i = 0; i < f; ++i) { const float g = static_cast<float>(i) / f; c.samples[i] *= g; c.samples[c.samples.size() - 1 - i] *= g; }
  r.ok = true; r.entry = std::move(c);
  r.message = what + ": " + std::to_string(static_cast<int>((s1 - s0) * 1000.0 / sr)) + " ms cut from after the vowel, " + std::to_string(static_cast<int>(20.0 * std::log10(crms / vrms))) + " dB relative to the vowel.";
  return r;
}

ConsonantResult makeConsonantEntry(const std::string& name, const AudioBuffer& audio, double sr) {
  ConsonantResult r;
  const std::string what = "\"" + name + "\"";
  if (!isConsonantName(name)) { r.message = what + " is not a consonant I know."; return r; }
  if (audio.frames < sr * 0.4) { r.message = "The recording for " + what + " is too short. Say the consonant followed by a held \"ah\" (for example \"" + name + "ah\")."; return r; }
  const auto fr = analyzeVoice(audio, sr);
  if (fr.size() < 8) { r.message = "Could not analyse the recording for " + what + "."; return r; }
  std::vector<double> f; for (auto& x : fr) if (x.voiced && x.f0 > 50 && x.f0 < 1200) f.push_back(x.f0);
  if (f.size() < 8) { r.message = "I found no held vowel in the recording for " + what + ". Say the consonant and then hold \"ah\" for a second."; return r; }
  std::sort(f.begin(), f.end()); const double med = f[f.size() / 2];
  std::size_t bestA = 0, bestN = 0, a = 0, n = 0;
  for (std::size_t i = 0; i <= fr.size(); ++i) {
    const bool ok = i < fr.size() && fr[i].voiced && std::fabs(fr[i].f0 / med - 1.0) < 0.06 && !fr[i].silent;
    if (ok) { if (!n) a = i; ++n; } else { if (n > bestN) { bestN = n; bestA = a; } n = 0; }
  }
  const double hop = fr[1].time - fr[0].time;
  if (bestN * hop < 0.25) { r.message = "The held vowel in the recording for " + what + " is too short (needs about a quarter of a second of steady \"ah\")."; return r; }
  const float* x = audio.channel(0);
  const double vowelStart = fr[bestA].time;
  // onset of sound: first 4 ms frame above -30 dB of the loudest part
  const std::size_t win = std::max<std::size_t>(1, static_cast<std::size_t>(sr * 0.004));
  std::vector<double> e; for (std::size_t i = 0; i + win <= audio.frames; i += win) { double s = 0; for (std::size_t k = 0; k < win; ++k) s += double(x[i + k]) * x[i + k]; e.push_back(std::sqrt(s / static_cast<double>(win))); }
  const double pk = *std::max_element(e.begin(), e.end());
  if (pk < 1e-5) { r.message = "The recording for " + what + " is silent."; return r; }
  std::size_t on = 0; while (on < e.size() && e[on] < pk * 0.03) ++on;
  const double t0 = std::max(0.0, static_cast<double>(on * win) / sr - 0.005);
  // voiceless consonants (p t k f s sh ch th h) are noise before the voice starts: they must really be unvoiced, and end where voicing begins.
  // voiced and sonorant ones (b d g v z j m n ng l r w y) are voiced themselves, so they run up to the steady vowel.
  static const char* voiceless[] = {"p", "t", "k", "f", "s", "sh", "ch", "th", "h"};
  bool unvoiced = false; for (const char* v : voiceless) if (name == v) unvoiced = true;
  double t1 = vowelStart;
  if (unvoiced) {
    double firstVoiced = vowelStart; std::size_t before = 0, voicedBefore = 0;
    for (const auto& fx : fr) if (fx.time >= t0 && fx.time < vowelStart) { ++before; if (fx.voiced) { ++voicedBefore; firstVoiced = std::min(firstVoiced, fx.time); } }
    if (before == 0 || voicedBefore * 2 > before) { r.message = "I could not hear the voiceless sound " + what + " before the vowel. Say it clearly, then hold \"ah\" (for example \"" + name + "ah\")."; return r; }
    t1 = firstVoiced + 0.008;
  }
  if (t1 - t0 < 0.02) { r.message = "There is almost nothing before the vowel in the recording for " + what + ". Say the consonant clearly before the \"ah\"."; return r; }
  const double len = std::min(t1 - t0, 0.26);
  const std::size_t s0 = static_cast<std::size_t>(t0 * sr), s1 = std::min(audio.frames, s0 + static_cast<std::size_t>(len * sr));
  ConsonantEntry c; c.name = name; c.rate = sr; c.samples.assign(x + s0, x + s1);
  double mean = 0; for (float v : c.samples) mean += v; mean /= static_cast<double>(c.samples.size());
  double cs = 0; for (float& v : c.samples) { v -= static_cast<float>(mean); cs += double(v) * v; }
  const double crms = std::sqrt(cs / static_cast<double>(c.samples.size()));
  // the vowel's loudness in the same recording
  const std::size_t v0 = static_cast<std::size_t>((vowelStart + 0.05) * sr), v1 = std::min(audio.frames, v0 + static_cast<std::size_t>(0.2 * sr));
  double vs = 0; for (std::size_t i = v0; i < v1; ++i) vs += double(x[i]) * x[i];
  const double vrms = v1 > v0 ? std::sqrt(vs / static_cast<double>(v1 - v0)) : 0.0;
  if (crms < 1e-6 || vrms < 1e-5) { r.message = "The recording for " + what + " is too quiet to use."; return r; }
  const double rel = std::min(2.5, crms / vrms);   // relative to the vowel
  const float g = static_cast<float>(rel * kBankRms / crms);
  for (float& v : c.samples) v *= g;
  const std::size_t fi = std::min<std::size_t>(c.samples.size() / 4, static_cast<std::size_t>(sr * 0.002)), fo = std::min<std::size_t>(c.samples.size() / 2, static_cast<std::size_t>(sr * 0.008));
  for (std::size_t i = 0; i < fi; ++i) c.samples[i] *= static_cast<float>(i) / static_cast<float>(fi);
  for (std::size_t i = 0; i < fo; ++i) c.samples[c.samples.size() - 1 - i] *= static_cast<float>(i) / static_cast<float>(fo);
  r.ok = true;
  r.message = what + ": " + std::to_string(static_cast<int>(len * 1000)) + " ms, " + std::to_string(static_cast<int>(std::lround(20 * std::log10(rel)))) + " dB relative to the vowel.";
  r.entry = std::move(c);
  return r;
}


namespace {
// Typical adult formants (F1, F2 in Hz) of the vowels the bank knows. Only used to warn about a mislabeled recording.
struct VowelRef { const char* name; double f1, f2; };
const VowelRef kVowelRefs[] = {{"ah", 730, 1090}, {"ee", 270, 2290}, {"oo", 300, 870}, {"oh", 570, 840}, {"eh", 530, 1840}, {"ih", 390, 1990}, {"uh", 640, 1190}, {"ae", 660, 1720}, {"er", 490, 1350}};
double vowelDistance(double f1, double f2, const VowelRef& r) { const double a = std::log2(f1 / r.f1), b = std::log2(f2 / r.f2); return std::sqrt(a * a + b * b); }
}  // namespace

std::string nearestVowel(double f1, double f2, double* distanceOctaves) {
  const VowelRef* best = nullptr; double bd = 1e9;
  if (f1 > 0 && f2 > 0) for (const auto& r : kVowelRefs) { const double d = vowelDistance(f1, f2, r); if (d < bd) { bd = d; best = &r; } }
  if (distanceOctaves) *distanceOctaves = bd;
  return best ? best->name : std::string();
}

std::string vowelLabelWarning(const std::string& label, double f1, double f2) {
  if (f1 <= 0 || f2 <= 0) return {};
  const VowelRef* best = nullptr; double bd = 1e9, ld = -1;
  for (const auto& r : kVowelRefs) { const double d = vowelDistance(f1, f2, r); if (d < bd) { bd = d; best = &r; } if (label == r.name) ld = d; }
  if (ld < 0 || !best || label == best->name || ld < bd + 0.2) return {};
  return "Warning: this recording measures F1 " + std::to_string(static_cast<int>(f1)) + " Hz, F2 " + std::to_string(static_cast<int>(f2)) + " Hz, which is closer to \"" + best->name + "\" than to \"" + label + "\". Check the label.";
}

BankResult makeBankEntry(const std::string& vowel, const AudioBuffer& audio, double sr, double minSteadySeconds) {
  BankResult r;
  const bool shortMode = minSteadySeconds < 0.3;   // a vowel from a spoken letter name: shorter, and its pitch drifts more
  if (audio.frames < sr * (shortMode ? 0.25 : 0.5)) { r.message = "The recording of \"" + vowel + "\" is too short (needs about 1.5 s of one steady note)."; return r; }
  const auto fr = analyzeVoice(audio, sr);
  if (fr.size() < 4) { r.message = "Could not analyse the recording of \"" + vowel + "\"."; return r; }
  std::vector<double> f; for (auto& x : fr) if (x.voiced && x.f0 > 50 && x.f0 < 1200) f.push_back(x.f0);
  if (f.size() < 8) { r.message = "I found almost no clear pitch in \"" + vowel + "\". Sing or hold one steady, dry note."; return r; }
  std::sort(f.begin(), f.end()); const double med = f[f.size() / 2];
  // longest run of voiced frames within 4 % of the median pitch
  std::size_t bestA = 0, bestN = 0, a = 0, n = 0;
  for (std::size_t i = 0; i <= fr.size(); ++i) {
    const bool ok = i < fr.size() && fr[i].voiced && std::fabs(fr[i].f0 / med - 1.0) < (shortMode ? 0.10 : 0.04) && !fr[i].silent;
    if (ok) { if (!n) a = i; ++n; } else { if (n > bestN) { bestN = n; bestA = a; } n = 0; }
  }
  const double hop = fr.size() > 1 ? fr[1].time - fr[0].time : 0.01;
  if (bestN * hop < minSteadySeconds) { r.message = "The steady part of \"" + vowel + "\" is shorter than " + std::to_string(minSteadySeconds).substr(0, 4) + " s. Hold the note steadier and longer."; return r; }
  // trim a little at both ends (onset and release are not steady)
  const double t0 = fr[bestA].time + 0.06, t1 = fr[bestA + bestN - 1].time - 0.04;
  std::size_t s0 = static_cast<std::size_t>(std::max(0.0, t0) * sr), s1 = std::min(audio.frames, static_cast<std::size_t>(std::max(t0 + 0.2, t1) * sr));
  const float* x = audio.channel(0);
  double f0 = 0; { double s = 0; int c = 0; for (std::size_t i = bestA; i < bestA + bestN; ++i) if (fr[i].time >= t0 && fr[i].time <= t1) { s += fr[i].f0; ++c; } f0 = c ? s / c : med; }
  BankEntry e; e.vowel = vowel; e.f0 = f0; e.rate = sr;
  e.samples.assign(x + s0, x + s1);
  // remove DC and normalize
  double mean = 0; for (float v : e.samples) mean += v; mean /= static_cast<double>(e.samples.size());
  double ss = 0; for (float& v : e.samples) { v -= static_cast<float>(mean); ss += static_cast<double>(v) * v; }
  const double rms = std::sqrt(ss / static_cast<double>(e.samples.size()));
  if (rms < 1e-5) { r.message = "The recording of \"" + vowel + "\" is silent."; return r; }
  for (float& v : e.samples) v = static_cast<float>(v * kBankRms / rms);
  // pitch marks: one positive peak per period, tracked from the loudest peak of the first period
  const double T = sr / f0; const int N = static_cast<int>(e.samples.size());
  int first = 0; { float b = -1e9f; for (int i = 0; i < std::min(N, static_cast<int>(T * 1.2)); ++i) if (e.samples[i] > b) { b = e.samples[i]; first = i; } }
  e.marks.push_back(first);
  while (true) {
    const int lo = e.marks.back() + static_cast<int>(T * 0.8), hi = e.marks.back() + static_cast<int>(T * 1.2);
    if (hi >= N) break;
    int bi = lo; float b = -1e9f; for (int i = lo; i <= hi; ++i) if (e.samples[i] > b) { b = e.samples[i]; bi = i; }
    e.marks.push_back(bi);
  }
  if (e.marks.size() < 6) { r.message = "Too few pitch periods in \"" + vowel + "\"."; return r; }
  r.ok = true; r.entry = std::move(e);
  std::string warn;
  { AudioBuffer seg(1, r.entry.samples.size()); std::copy(r.entry.samples.begin(), r.entry.samples.end(), seg.channel(0));
    double f1 = 0, f2 = 0; if (measureFormants(seg, r.entry.rate, f1, f2)) warn = vowelLabelWarning(vowel, f1, f2); }
  r.message = "\"" + vowel + "\": " + std::to_string(static_cast<int>(std::lround(f0))) + " Hz, " + std::to_string(static_cast<int>((s1 - s0) * 1000 / sr)) + " ms steady." + (warn.empty() ? std::string() : " " + warn);
  return r;
}

// ------------------------------------------------------------ file
namespace {
constexpr char kMagic[8] = {'W', 'V', 'B', 'N', 'K', '0', '2', 0};   // 02 adds consonants; 01 files still load
constexpr char kMagicV1[8] = {'W', 'V', 'B', 'N', 'K', '0', '1', 0};
void putStr(std::ofstream& o, const std::string& s) { std::uint32_t n = static_cast<std::uint32_t>(s.size()); o.write(reinterpret_cast<const char*>(&n), 4); o.write(s.data(), n); }
bool getStr(std::ifstream& i, std::string& s) { std::uint32_t n = 0; if (!i.read(reinterpret_cast<char*>(&n), 4) || n > (1u << 20)) return false; s.resize(n); return n == 0 || static_cast<bool>(i.read(s.data(), n)); }
}  // namespace

bool saveVoiceBank(const std::string& path, const VoiceBank& b, std::string* error) {
  std::ofstream o(path, std::ios::binary);
  if (!o) { if (error) *error = "cannot write " + path; return false; }
  o.write(kMagic, 8); putStr(o, b.name); putStr(o, b.owner); putStr(o, b.consent);
  const std::uint32_t n = static_cast<std::uint32_t>(b.entries.size()); o.write(reinterpret_cast<const char*>(&n), 4);
  for (const auto& e : b.entries) {
    putStr(o, e.vowel); o.write(reinterpret_cast<const char*>(&e.f0), 8); o.write(reinterpret_cast<const char*>(&e.rate), 8);
    const std::uint32_t ns = static_cast<std::uint32_t>(e.samples.size()), nm = static_cast<std::uint32_t>(e.marks.size());
    o.write(reinterpret_cast<const char*>(&ns), 4); o.write(reinterpret_cast<const char*>(e.samples.data()), static_cast<std::streamsize>(ns) * 4);
    o.write(reinterpret_cast<const char*>(&nm), 4); o.write(reinterpret_cast<const char*>(e.marks.data()), static_cast<std::streamsize>(nm) * 4);
  }
  const std::uint32_t nc = static_cast<std::uint32_t>(b.consonants.size()); o.write(reinterpret_cast<const char*>(&nc), 4);
  for (const auto& c : b.consonants) {
    putStr(o, c.name); o.write(reinterpret_cast<const char*>(&c.rate), 8);
    const std::uint32_t ns = static_cast<std::uint32_t>(c.samples.size()); o.write(reinterpret_cast<const char*>(&ns), 4); o.write(reinterpret_cast<const char*>(c.samples.data()), static_cast<std::streamsize>(ns) * 4);
  }
  if (!o) { if (error) *error = "write failed"; return false; }
  return true;
}

bool loadVoiceBank(const std::string& path, VoiceBank& b, std::string* error) {
  auto fail = [&](const char* m) { if (error) *error = m; return false; };
  std::ifstream i(path, std::ios::binary);
  if (!i) return fail("file not found");
  char mg[8]; if (!i.read(mg, 8)) return fail("not a Wezt voice bank");
  const bool v2 = std::memcmp(mg, kMagic, 8) == 0;
  if (!v2 && std::memcmp(mg, kMagicV1, 8) != 0) return fail("not a Wezt voice bank");
  VoiceBank v;
  std::uint32_t n = 0;
  if (!getStr(i, v.name) || !getStr(i, v.owner) || !getStr(i, v.consent) || !i.read(reinterpret_cast<char*>(&n), 4) || n > 64) return fail("damaged file");
  for (std::uint32_t k = 0; k < n; ++k) {
    BankEntry e; std::uint32_t ns = 0, nm = 0;
    if (!getStr(i, e.vowel) || !i.read(reinterpret_cast<char*>(&e.f0), 8) || !i.read(reinterpret_cast<char*>(&e.rate), 8) || !i.read(reinterpret_cast<char*>(&ns), 4) || ns > 20000000) return fail("damaged file");
    if (!(e.f0 > 20 && e.f0 < 3000) || !(e.rate >= 8000 && e.rate <= 384000)) return fail("damaged file");
    e.samples.resize(ns); if (!i.read(reinterpret_cast<char*>(e.samples.data()), static_cast<std::streamsize>(ns) * 4)) return fail("damaged file");
    if (!i.read(reinterpret_cast<char*>(&nm), 4) || nm > ns) return fail("damaged file");
    e.marks.resize(nm); if (!i.read(reinterpret_cast<char*>(e.marks.data()), static_cast<std::streamsize>(nm) * 4)) return fail("damaged file");
    for (int m : e.marks) if (m < 0 || static_cast<std::uint32_t>(m) >= ns) return fail("damaged file");
    for (float s : e.samples) if (!std::isfinite(s)) return fail("damaged file");
    v.entries.push_back(std::move(e));
  }
  if (v2) {
    std::uint32_t nc = 0;
    if (!i.read(reinterpret_cast<char*>(&nc), 4) || nc > 64) return fail("damaged file");
    for (std::uint32_t k = 0; k < nc; ++k) {
      ConsonantEntry c; std::uint32_t ns = 0;
      if (!getStr(i, c.name) || !i.read(reinterpret_cast<char*>(&c.rate), 8) || !i.read(reinterpret_cast<char*>(&ns), 4) || ns > 2000000) return fail("damaged file");
      if (!isConsonantName(c.name) || !(c.rate >= 8000 && c.rate <= 384000)) return fail("damaged file");
      c.samples.resize(ns); if (!i.read(reinterpret_cast<char*>(c.samples.data()), static_cast<std::streamsize>(ns) * 4)) return fail("damaged file");
      for (float s : c.samples) if (!std::isfinite(s)) return fail("damaged file");
      v.consonants.push_back(std::move(c));
    }
  }
  b = std::move(v);
  return true;
}

// ------------------------------------------------------------ PSOLA player
BankVoice::BankVoice(const BankEntry& e, double outRate) : rate_(outRate) {
  const double ratio = outRate / e.rate;
  if (std::fabs(ratio - 1.0) < 1e-6) { src_ = e.samples; marks_ = e.marks; }
  else {
    const std::size_t n = static_cast<std::size_t>(static_cast<double>(e.samples.size()) * ratio);
    src_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double p = static_cast<double>(i) / ratio; const std::size_t k = static_cast<std::size_t>(p); const double fr = p - static_cast<double>(k);
      const float a = e.samples[std::min(k, e.samples.size() - 1)], b = e.samples[std::min(k + 1, e.samples.size() - 1)];
      src_[i] = static_cast<float>(a + (b - a) * fr);
    }
    for (int m : e.marks) { const int q = static_cast<int>(std::lround(m * ratio)); if (q >= 0 && static_cast<std::size_t>(q) < n) marks_.push_back(q); }
  }
  period_ = outRate / e.f0;
  ring_.assign(static_cast<std::size_t>(period_ * 8 + 4096), 0.f);
  srcPos_ = marks_.size() > 4 ? 2.0 : 0.0;
}

void BankVoice::grain(double f0) {
  // a grain is two recorded periods around a mark, Hann-windowed, added into the ring at the read position
  if (marks_.size() < 4) return;
  const std::size_t mi = static_cast<std::size_t>(std::clamp(srcPos_, 1.0, static_cast<double>(marks_.size() - 2)));
  const int c = marks_[mi], a = marks_[mi - 1], b = marks_[mi + 1];
  const int L = c - a, R = b - c;
  const std::size_t N = ring_.size();
  const double gain = (rate_ / f0) / (0.5 * (L + R));   // Hann grains of two periods at spacing P sum to T/P
  const std::size_t D = static_cast<std::size_t>(period_ * 1.3) + 8;   // fixed delay: a grain reaches back L samples before its centre
  for (int k = -L; k < R; ++k) {
    const int si = c + k; if (si < 0 || static_cast<std::size_t>(si) >= src_.size()) continue;
    const double w = k < 0 ? 0.5 * (1 + std::cos(M_PI * k / static_cast<double>(L))) : 0.5 * (1 + std::cos(M_PI * k / static_cast<double>(R)));
    ring_[(rd_ + D + N + static_cast<std::size_t>(k + static_cast<int>(N)) - N) % N] += static_cast<float>(src_[static_cast<std::size_t>(si)] * w * gain);
  }
  // when the output is higher than the recording, periods are skipped; lower: repeated. Walk slowly back and forth
  // through the recording so repeated grains are not identical (looping artifacts)
  srcPos_ += dir_ * 1.0;
  if (srcPos_ >= static_cast<double>(marks_.size() - 2)) { srcPos_ = static_cast<double>(marks_.size() - 3); dir_ = -1; }
  if (srcPos_ <= 1.0) { srcPos_ = 1.0 + 1.0; dir_ = 1; }
}

double BankVoice::next(double f0) {
  f0 = std::clamp(f0, 40.0, 2000.0);
  if (untilGrain_ <= 0) {
    // the output ring position of a new grain starts L samples before its centre: keep the centre aligned with "now"
    grain(f0);
    untilGrain_ += rate_ / f0;
  }
  untilGrain_ -= 1.0;
  const std::size_t N = ring_.size();
  const float v = ring_[rd_];
  ring_[rd_] = 0.f; rd_ = (rd_ + 1) % N;
  return v;
}

}  // namespace wezt::voice
