#include "voice/singing.h"

#include <algorithm>
#include <memory>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "dsp/biquad.h"
#include "music/theory.h"

namespace wezt::voice {
namespace {

bool isVowelCh(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y'; }

// ---------------------------------------------------------------- spelling -> syllables
std::string vowelFor(const std::string& g, const std::string& after, bool wordEnd) {
  static const std::vector<std::pair<std::string, std::string>> digraphs = {
      {"igh", "ah"}, {"ee", "ee"}, {"ea", "ee"}, {"ie", "ee"}, {"ey", "ee"}, {"oo", "oo"}, {"ew", "oo"}, {"ou", "oh"}, {"ow", "oh"}, {"oa", "oh"}, {"oe", "oh"},
      {"ai", "eh"}, {"ay", "eh"}, {"ei", "eh"}, {"au", "oh"}, {"aw", "oh"}, {"ue", "oo"}, {"ui", "oo"}};
  for (auto& [k, v] : digraphs) if (g == k) return v;
  if (g.size() > 1) return vowelFor(g.substr(0, 1), after, wordEnd);
  const char c = g[0];
  const bool r = !after.empty() && after[0] == 'r';
  if (r) return c == 'a' ? "ah" : c == 'o' ? "oh" : "er";
  switch (c) {
    case 'a': return after.empty() ? "ah" : "ae";
    case 'e': return "eh";
    case 'i': return "ih";
    case 'o': return after.empty() ? "oh" : "oh";
    case 'u': return after.empty() ? "oo" : "uh";
    case 'y': return wordEnd ? "ee" : "ih";
  }
  return "ah";
}

std::vector<std::string> splitWord(const std::string& w) {
  // vowel groups with their spans
  std::string s = w;
  // silent final e after a consonant in a word with another vowel group
  auto groups = [&](const std::string& x) { int n = 0; bool in = false; for (char c : x) { const bool v = isVowelCh(c); if (v && !in) ++n; in = v; } return n; };
  if (s.size() > 2 && s.back() == 'e' && !isVowelCh(s[s.size() - 2]) && groups(s) > 1) s.pop_back();
  std::vector<std::pair<std::size_t, std::size_t>> vg;
  for (std::size_t i = 0; i < s.size();) {
    if (isVowelCh(s[i]) && !(s[i] == 'y' && i == 0 && s.size() > 1 && isVowelCh(s[1]) == false && false)) {
      std::size_t j = i; while (j < s.size() && isVowelCh(s[j])) ++j;
      vg.push_back({i, j}); i = j;
    } else ++i;
  }
  if (vg.size() <= 1) return {s};
  std::vector<std::string> out;
  std::size_t start = 0;
  auto digraphAt = [&](std::size_t i) { if (i + 1 >= s.size()) return false; const std::string d = s.substr(i, 2); return d == "sh" || d == "ch" || d == "th" || d == "ng" || d == "ck" || d == "ph" || d == "wh"; };
  for (std::size_t g = 0; g + 1 < vg.size(); ++g) {
    const std::size_t a = vg[g].second, b = vg[g + 1].first;   // consonants between
    const std::size_t n = b - a;
    std::size_t cut;
    if (n == 0) cut = vg[g].second;                  // two vowel groups touching (rare)
    else if (n == 1) cut = a;                        // V-CV
    else { cut = a + n / 2; if (digraphAt(cut - 1) && cut > a) cut -= 1; else if (digraphAt(cut) && cut + 2 <= b) { /* keep digraph with next */ } }
    out.push_back(s.substr(start, cut - start));
    start = cut;
  }
  out.push_back(s.substr(start));
  return out;
}

Syllable analyse(const std::string& text, bool wordStart, bool wordEnd) {
  Syllable sy; sy.text = text; sy.wordStart = wordStart;
  // explicit vowel: "la[ah]" is handled before this; here plain letters
  std::size_t i = 0;
  while (i < text.size() && !isVowelCh(text[i])) ++i;
  if (i < text.size() && text[i] == 'y' && i > 0 && false) {}
  sy.onset = text.substr(0, i);
  std::size_t j = i; while (j < text.size() && isVowelCh(text[j])) ++j;
  std::string group = text.substr(i, j - i);
  sy.coda = text.substr(j);
  if (group.empty()) { sy.vowel = "uh"; return sy; }
  // 'w' before a vowel group like "ow"/"aw" is a consonant-vowel mix; keep it simple
  if (group == "i" && sy.coda.compare(0, 2, "gh") == 0) { sy.vowel = "ah"; sy.coda = sy.coda.substr(2); return sy; }   // light, night
  sy.vowel = vowelFor(group, sy.coda, wordEnd);
  if (group == "e" && sy.coda.empty() && !wordStart && wordEnd) sy.vowel = "ee";   // "be", "me"
  if (sy.coda == "ng" && sy.vowel == "ae") sy.vowel = "ih";
  return sy;
}

// ---------------------------------------------------------------- formants
struct Formants { double f[5], bw[5], gain; };
const std::map<std::string, Formants>& vowels() {
  static const std::map<std::string, Formants> v = {
      {"ah", {{730, 1090, 2440, 3400, 4500}, {80, 90, 120, 130, 140}, 1.0}},
      {"ee", {{270, 2290, 3010, 3600, 4500}, {60, 90, 130, 150, 160}, 0.95}},
      {"eh", {{530, 1840, 2480, 3500, 4500}, {70, 90, 120, 140, 150}, 1.0}},
      {"ae", {{660, 1720, 2410, 3400, 4500}, {80, 90, 120, 140, 150}, 1.0}},
      {"ih", {{390, 1990, 2550, 3500, 4500}, {70, 90, 120, 140, 150}, 1.0}},
      {"oh", {{450, 800, 2830, 3500, 4500}, {70, 80, 120, 140, 150}, 1.05}},
      {"oo", {{300, 870, 2240, 3400, 4500}, {60, 80, 110, 140, 150}, 1.1}},
      {"uh", {{640, 1190, 2390, 3400, 4500}, {80, 90, 120, 140, 150}, 1.0}},
      {"er", {{490, 1350, 1690, 3300, 4500}, {80, 100, 120, 140, 150}, 1.0}},
  };
  return v;
}

struct Res {
  double a1 = 0, a2 = 0, g = 1, y1 = 0, y2 = 0;
  void set(double f, double bw, double sr, bool dcUnity = true) {
    f = std::clamp(f, 40.0, sr * 0.45);
    const double r = std::exp(-M_PI * bw / sr);
    a1 = 2 * r * std::cos(2 * M_PI * f / sr); a2 = -r * r;
    // gain that makes the peak of this resonator exactly 1, whatever its frequency and bandwidth
    const double th = 2 * M_PI * f / sr;
    const double re = 1 - a1 * std::cos(th) - a2 * std::cos(2 * th), im = a1 * std::sin(th) + a2 * std::sin(2 * th);
    g = dcUnity ? 1 - a1 - a2 : std::sqrt(re * re + im * im);   // formants: unity at 0 Hz (so a cascade keeps the voice's fundamental); noise bands: unity at the centre
  }
  double run(double x) { const double y = g * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
};

struct Pitchless { double phase = 0; };

struct Consonant { double dur; bool voiced; double noiseHz, noiseBw, noiseAmp, voiceAmp; const ConsonantEntry* rec = nullptr; };
bool consonant(const std::string& c, Consonant& out) {
  // c is a letter or a digraph
  static const std::map<std::string, Consonant> m = {
      {"s", {0.085, false, 6500, 2500, 0.9, 0}}, {"z", {0.06, true, 6000, 2500, 0.35, 0.5}}, {"sh", {0.09, false, 3200, 1500, 1.0, 0}}, {"ch", {0.08, false, 3200, 1500, 1.0, 0}},
      {"f", {0.07, false, 5000, 4000, 0.45, 0}}, {"v", {0.055, true, 5000, 4000, 0.25, 0.5}}, {"th", {0.07, false, 5500, 4000, 0.3, 0}}, {"h", {0.06, false, 1800, 1500, 0.5, 0}},
      {"t", {0.03, false, 4500, 2500, 0.8, 0}}, {"k", {0.035, false, 2200, 1500, 0.8, 0}}, {"p", {0.03, false, 1200, 1500, 0.8, 0}}, {"c", {0.035, false, 2200, 1500, 0.8, 0}},
      {"b", {0.04, true, 900, 1200, 0.3, 0.6}}, {"d", {0.04, true, 3000, 2500, 0.3, 0.6}}, {"g", {0.04, true, 1800, 1500, 0.3, 0.6}}, {"j", {0.06, true, 3000, 1500, 0.4, 0.5}},
      {"m", {0.075, true, 0, 0, 0, 0.7}}, {"n", {0.07, true, 0, 0, 0, 0.7}}, {"ng", {0.07, true, 0, 0, 0, 0.7}}, {"l", {0.06, true, 0, 0, 0, 0.8}}, {"r", {0.06, true, 0, 0, 0, 0.8}},
      {"w", {0.05, true, 0, 0, 0, 0.8}}, {"y", {0.05, true, 0, 0, 0, 0.8}}, {"q", {0.035, false, 2200, 1500, 0.8, 0}}, {"x", {0.08, false, 5000, 3000, 0.6, 0}},
  };
  auto it = m.find(c);
  if (it == m.end()) return false;
  out = it->second;
  return true;
}

// A consonant from the person's own recording replaces the synthesized one (and sets its length).
void useRecording(const VoiceBank* bank, const std::string& name, Consonant& cc, SingResult& res) {
  if (!bank) return;
  const ConsonantEntry* r = bank->findConsonant(name);
  if (!r || r->samples.empty() || r->rate <= 0) { ++res.bankConsonantsSynth; return; }
  cc.rec = r; cc.dur = std::clamp(static_cast<double>(r->samples.size()) / r->rate, 0.02, 0.26); ++res.bankConsonants;
}
// Plays the recording into out[a..b): a little faster when the slot is shorter than it was spoken.
void playRecording(const ConsonantEntry& r, std::vector<float>& out, std::size_t a, std::size_t b, double sr, double gain) {
  if (b <= a) return;
  const double natural = static_cast<double>(r.samples.size()) / r.rate, d = static_cast<double>(b - a) / sr;
  const double speed = std::clamp(natural / d, 1.0, 1.6) * r.rate / sr;   // source samples per output sample
  const double n = static_cast<double>(r.samples.size() - 1);
  const std::size_t fade = std::max<std::size_t>(1, static_cast<std::size_t>(sr * 0.004));
  for (std::size_t i = a; i < b; ++i) {
    const double pos = static_cast<double>(i - a) * speed;
    if (pos >= n) break;
    const std::size_t k = static_cast<std::size_t>(pos); const double fr = pos - static_cast<double>(k);
    double s = r.samples[k] + (r.samples[k + 1] - r.samples[k]) * fr;
    const std::size_t left = b - i;
    if (left < fade) s *= static_cast<double>(left) / static_cast<double>(fade);   // when truncated
    out[i] += static_cast<float>(s * gain);
  }
}

std::vector<std::string> consonantList(const std::string& letters) {
  std::vector<std::string> v;
  for (std::size_t i = 0; i < letters.size(); ++i) {
    if (i + 1 < letters.size()) { const std::string d = letters.substr(i, 2); if (d == "sh" || d == "ch" || d == "th" || d == "ng") { v.push_back(d); ++i; continue; } if (d == "ck") { v.push_back("k"); ++i; continue; } if (d == "ph") { v.push_back("f"); ++i; continue; } if (d == "wh") { v.push_back("w"); ++i; continue; } }
    if (letters[i] == letters[i > 0 ? i - 1 : 0] && i > 0) continue;   // double letters sound once
    v.push_back(std::string(1, letters[i]));
  }
  return v;
}

// formants used while a nasal/liquid/glide consonant is voiced
Formants murmur(const std::string& c) {
  if (c == "m" || c == "n" || c == "ng") return {{250, 1000, 2200, 3300, 4500}, {100, 150, 200, 200, 200}, 0.55};
  if (c == "l") return {{360, 1300, 2800, 3500, 4500}, {80, 120, 150, 150, 150}, 0.8};
  if (c == "r") return vowels().at("er");
  if (c == "w") return vowels().at("oo");
  if (c == "y") return vowels().at("ee");
  return {{300, 1200, 2400, 3400, 4500}, {120, 150, 180, 200, 200}, 0.5};   // voiced stops/fricatives
}

struct Voice { double scale, lo, hi; };
Voice voiceInfo(VoiceType v) {
  switch (v) {
    case VoiceType::Bass: return {0.88, 40, 62};
    case VoiceType::Tenor: return {1.0, 48, 69};
    case VoiceType::Alto: return {1.12, 55, 74};
    case VoiceType::Soprano: return {1.2, 60, 84};
  }
  return {1.0, 48, 69};
}

struct NoteEv { double start = 0, end = 0; int pitch = 60; int syl = 0; bool melisma = false; int vel = 90; };
constexpr double kUpperBw[5] = {1.0, 1.0, 1.0, 1.0, 1.0};   // (resonators are normalized at DC, so widening an upper formant lowers its peak and darkens the voice: measured, then undone)
constexpr double kBankLevel = 11.0;   // recorded vowels are stored at RMS 0.1; x11 puts them at the loudness of the synthesized "ee" and "oo" (raw RMS about 0.5; "ah" is louder), measured, so synthesized consonants sit right next to them
constexpr double kSource = 0.337;   // level of the voiced sound relative to the consonant noises (calibrated against the previous voice)

}  // namespace

const char* voiceName(VoiceType v) { return v == VoiceType::Bass ? "bass" : v == VoiceType::Tenor ? "tenor" : v == VoiceType::Alto ? "alto" : "soprano"; }
bool parseVoice(const std::string& s, VoiceType& out) {
  std::string l; for (char c : s) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (l == "bass" || l == "male low") out = VoiceType::Bass;
  else if (l == "tenor" || l == "male") out = VoiceType::Tenor;
  else if (l == "alto" || l == "female low") out = VoiceType::Alto;
  else if (l == "soprano" || l == "female") out = VoiceType::Soprano;
  else return false;
  return true;
}

std::vector<Syllable> syllabify(const std::string& lyrics) {
  std::vector<Syllable> out;
  std::string word; bool hyphenCont = false;
  auto flush = [&]() {
    if (word.empty()) return;
    // explicit vowel tags: pieces like "la[ah]"
    std::string forced;
    std::string clean;
    for (std::size_t i = 0; i < word.size(); ++i) {
      if (word[i] == '[') { const auto e = word.find(']', i); if (e != std::string::npos) { forced = word.substr(i + 1, e - i - 1); i = e; continue; } }
      clean += word[i];
    }
    auto pieces = splitWord(clean);
    for (std::size_t k = 0; k < pieces.size(); ++k) {
      Syllable s = analyse(pieces[k], k == 0 && !hyphenCont, k + 1 == pieces.size());
      if (!forced.empty() && k + 1 == pieces.size() && vowels().count(forced)) s.vowel = forced;
      out.push_back(std::move(s));
    }
    word.clear(); hyphenCont = false;
  };
  std::string cur;
  for (char c : lyrics) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalpha(u) || c == '[' || c == ']') cur += static_cast<char>(std::tolower(u));
    else if (c == '\'') {}                       // don't -> dont
    else if (c == '-') { // syllable break inside a word: flush this part as its own word piece
      word = cur; cur.clear();
      if (!word.empty()) { const bool wasCont = hyphenCont; flush(); hyphenCont = true; (void)wasCont; }
      else hyphenCont = true;
    } else { word = cur; cur.clear(); flush(); }
  }
  word = cur; flush();
  return out;
}

// ---------------------------------------------------------------- the humanized voice
// What makes a held vowel sound like a person and not a machine, in the order it matters most:
//  * the pitch is never a straight line: singers scoop up into a note, overshoot after a leap, drift a few
//    cents, fall away at the end, and vibrato starts late, wanders in rate and depth, and moves loudness too;
//  * every glottal cycle differs a little from the last (jitter in period, shimmer in height), plus airy noise that
//    is pulsed with the opening of the vocal folds;
//  * louder singing is brighter, softer singing is breathier, and trained voices have a ring near 3 kHz;
//  * a real voice sits in a room, and the notes do not begin exactly on the grid.
// All of it is driven by a seeded random generator, so the same seed gives the same performance.
namespace {

double gauss(music::Rng& r) {
  const double u1 = std::max(1e-12, r.uniform()), u2 = r.uniform();
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
}

// Slowly wandering random signal of about unit variance (white noise through a one-pole low-pass).
struct Wander {
  double y = 0, a = 0, g = 1;
  void set(double cornerHz, double rate) { a = std::exp(-2.0 * M_PI * cornerHz / rate); g = std::sqrt(1.0 - a * a); }
  double next(music::Rng& r) { y = a * y + g * gauss(r); return y; }
};

double smooth01(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Glottal source: the derivative of the airflow through the vocal folds (the flow pulse is the
// Rosenberg shape; its derivative already includes the radiation from the lips, so there is no DC).
// `tc` is how fast the folds close (small = bright), `to` where the open phase ends.
struct Glottal {
  double phase = 0, periodScale = 1, amp = 1, lp = 0;
  double jitter = 0.0035, shimmer = 0.012;     // standard deviation of the cycle-to-cycle changes
  // Returns the source sample; `flow` (0..1) is the airflow, used to pulse the breath noise.
  double next(music::Rng& rng, double f, double sr, double to, double tc, double norm, double lpA, double& flow) {
    phase += f * periodScale / sr;
    if (phase >= 1.0) {
      phase -= std::floor(phase);
      periodScale = 1.0 / (1.0 + jitter * gauss(rng));   // this cycle is a little longer or shorter than the last
      amp = std::max(0.2, 1.0 + shimmer * gauss(rng));   // and a little taller or smaller
    }
    double d = 0;
    flow = 0;
    if (phase < to) { const double x = M_PI * phase / to; flow = 0.5 * (1.0 - std::cos(x)); d = 0.5 * (M_PI / to) * std::sin(x); }
    else if (phase < to + tc) { const double x = 0.5 * M_PI * (phase - to) / tc; flow = std::cos(x); d = -(0.5 * M_PI / tc) * std::sin(x); }
    lp += lpA * (d * norm * amp - lp);                    // spectral tilt: the source loses its top end as the voice gets softer
    return lp;
  }
};

// RMS of the source's derivative over one cycle, so every voice quality has the same level.
double sourceRms(double to, double tc) {
  double s = 0;
  const int N = 256;
  for (int i = 0; i < N; ++i) {
    const double ph = (i + 0.5) / N;
    double d = 0;
    if (ph < to) d = 0.5 * (M_PI / to) * std::sin(M_PI * ph / to);
    else if (ph < to + tc) d = -(0.5 * M_PI / tc) * std::sin(0.5 * M_PI * (ph - to) / tc);
    s += d * d;
  }
  return std::sqrt(s / N);
}

// A small room: four damped combs and two all-passes (Schroeder), mono.
struct Room {
  struct Comb { std::vector<float> b; std::size_t i = 0; float lp = 0; };
  std::vector<Comb> combs;
  struct Ap { std::vector<float> b; std::size_t i = 0; };
  std::vector<Ap> aps;
  float fb = 0.8f, damp = 0.3f;
  explicit Room(double sr, double size) {
    static const int cd[4] = {1116, 1188, 1277, 1356}, ad[2] = {225, 556};
    const double k = sr / 44100.0 * (0.8 + 0.6 * size);
    for (int d : cd) { Comb c; c.b.assign(static_cast<std::size_t>(d * k), 0.f); combs.push_back(std::move(c)); }
    for (int d : ad) { Ap a; a.b.assign(static_cast<std::size_t>(d * k), 0.f); aps.push_back(std::move(a)); }
    fb = static_cast<float>(0.72 + 0.14 * size);
  }
  float run(float x) {
    float acc = 0.f;
    for (auto& c : combs) {
      const float y = c.b[c.i];
      c.lp = y * (1.f - damp) + c.lp * damp;
      c.b[c.i] = x * 0.03f + c.lp * fb;
      if (++c.i >= c.b.size()) c.i = 0;
      acc += y;
    }
    for (auto& a : aps) {
      const float y = a.b[a.i];
      const float v = acc + y * 0.5f;
      a.b[a.i] = v;
      acc = y - 0.5f * v;
      if (++a.i >= a.b.size()) a.i = 0;
    }
    return acc;
  }
};

}  // namespace

namespace {
struct Character { const char* name; VoiceType v; double shift, bright, rough, fry, effort, breath, vibHz, vibCents, ring; };
const Character kCharacters[] = {
  // name               type              shift bright rough fry   effort breath vibHz vibC ring
  {"warm_alto",         VoiceType::Alto,    1.00, 0.70, 0.00, 0.10, -0.05, 0.05, 5.6, 22, 3.0},
  {"bright_pop",        VoiceType::Alto,    1.03, 1.35, 0.00, 0.05,  0.08, 0.03, 6.0, 14, 4.5},
  {"breathy_pop",       VoiceType::Alto,    1.00, 0.80, 0.00, 0.20, -0.18, 0.16, 5.4, 10, 1.5},
  {"gritty_rock",       VoiceType::Tenor,   0.96, 1.20, 0.55, 0.25,  0.22, 0.04, 5.2, 18, 6.0},
  {"operatic_soprano",  VoiceType::Soprano, 1.00, 1.10, 0.00, 0.00,  0.20, 0.02, 5.9, 60, 9.0},
  {"deep_bass",         VoiceType::Bass,    0.94, 0.65, 0.12, 0.30,  0.00, 0.04, 5.3, 28, 4.0},
  {"tenor_crooner",     VoiceType::Tenor,   1.00, 0.75, 0.00, 0.15, -0.06, 0.07, 5.8, 30, 3.5},
  {"boyish",            VoiceType::Tenor,   1.14, 1.10, 0.05, 0.00,  0.05, 0.05, 5.6, 12, 4.0},
  {"child",             VoiceType::Soprano, 1.28, 1.20, 0.00, 0.00,  0.00, 0.06, 5.0,  8, 2.0},
  {"airy_whisper",      VoiceType::Alto,    1.04, 0.90, 0.00, 0.00, -0.28, 0.30, 4.8,  6, 0.0},
  {"choirboy",          VoiceType::Soprano, 1.16, 1.00, 0.00, 0.00,  0.05, 0.03, 6.0, 40, 6.0},
  {"folk_storyteller",  VoiceType::Tenor,   0.98, 0.85, 0.20, 0.30, -0.02, 0.08, 5.0, 12, 2.5},
};
}  // namespace
bool applyCharacter(const std::string& name, SingParams& p) {
  std::string n; for (char c : name) n += c == ' ' || c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const Character& c : kCharacters) if (n == c.name) {
    p.voice = c.v; p.formantShift = c.shift; p.brightness = c.bright; p.roughness = c.rough; p.fry = c.fry;
    p.effortBias = c.effort; p.breath = c.breath; p.vibratoHz = c.vibHz; p.vibratoCents = c.vibCents; p.singerFormantDb = c.ring;
    return true;
  }
  return false;
}
std::vector<std::string> characterNames() { std::vector<std::string> v; for (const Character& c : kCharacters) v.push_back(c.name); return v; }

SingResult sing(const std::string& lyrics, const music::MidiClip& melody, const SingParams& p) {
  SingResult res;
  const double sr = p.sampleRate;
  auto syls = syllabify(lyrics);
  if (syls.empty()) { res.message = "I need lyrics with at least one word to sing."; return res; }
  if (melody.notes.empty()) { res.message = "The melody has no notes."; return res; }
  if (p.bpm <= 0) { res.message = "The tempo must be above zero."; return res; }
  const double spb = 60.0 / p.bpm;
  const double H = std::clamp(p.humanize, 0.0, 1.0);

  // one note at a time: when several start together keep the highest
  std::vector<music::Note> notes;
  {
    auto sorted = melody.notes;
    std::sort(sorted.begin(), sorted.end(), [](const music::Note& a, const music::Note& b) { return a.start < b.start || (a.start == b.start && a.pitch > b.pitch); });
    for (auto& n : sorted) if (notes.empty() || n.start > notes.back().start + 1e-6) notes.push_back(n);
  }
  const Voice vi = voiceInfo(p.voice);
  int shift = 0;
  if (p.fitRange) {
    std::vector<int> ps; for (auto& n : notes) ps.push_back(n.pitch);
    std::sort(ps.begin(), ps.end());
    const double med = ps[ps.size() / 2], centre = 0.5 * (vi.lo + vi.hi);
    shift = static_cast<int>(std::lround((centre - med) / 12.0)) * 12;
  }
  res.octaveShift = shift / 12;

  std::vector<NoteEv> evs;
  int si = 0;
  for (std::size_t i = 0; i < notes.size(); ++i) {
    NoteEv e; e.start = notes[i].start * spb; e.end = (notes[i].start + notes[i].length) * spb; e.pitch = std::clamp(notes[i].pitch + shift, 0, 127);
    e.vel = notes[i].velocity;
    if (si < static_cast<int>(syls.size())) { e.syl = si++; e.melisma = false; }
    else { e.syl = static_cast<int>(syls.size()) - 1; e.melisma = true; }
    evs.push_back(e);
  }
  res.syllables = static_cast<int>(syls.size()); res.notes = static_cast<int>(notes.size()); res.usedSyllables = si;

  const double tail = 0.6 + 1.2 * p.room;
  const std::size_t total = static_cast<std::size_t>((evs.back().end + 0.2 + tail) * sr);
  AudioBuffer mix(1, total);
  const int nv = std::clamp(p.choirVoices, 1, 8);
  const double dt = 1.0 / sr;

  for (int v = 0; v < nv; ++v) {
    music::Rng rng(p.seed * 131 + static_cast<std::uint64_t>(v) * 7919);
    const bool choir = nv > 1;
    // each singer in a choir is a different person: a little out of tune, early or late, with their own vibrato and tone
    const double detune = choir ? gauss(rng) * 9.0 : 0.0;                                   // cents
    const double delay = choir ? rng.uniform() * 0.035 : 0.0;                               // seconds
    const double fscale = vi.scale * std::clamp(p.formantShift, 0.6, 1.5) * (1.0 + (choir ? gauss(rng) * 0.025 : 0.0));
    const double vibRate = p.vibratoHz * (choir ? 0.88 + 0.24 * rng.uniform() : 1.0 + 0.04 * (rng.uniform() - 0.5));
    const double vibScale = choir ? 0.7 + 0.6 * rng.uniform() : 1.0;
    const double effortOffset = choir ? gauss(rng) * 0.07 : 0.0;
    const double voiceGain = choir ? 0.8 + 0.3 * rng.uniform() : 1.0;
    std::vector<float> out(total, 0.f);
    double vibPhase = rng.uniform() * 2.0 * M_PI;
    double prevF0 = 0; std::string prevVowel; double prevEnd = -1;
    Res fr[5];
    Glottal gl; gl.jitter = 0.0025 + 0.0008 * H + 0.004 * p.roughness; double roughPhase = rng.uniform() * 6.283, frySince = 0, fryGap = 0.03; gl.shimmer = 0.006 + 0.007 * H;
    if (H <= 0.0) { gl.jitter = 0; gl.shimmer = 0; }
    Wander drift, rateW, loud, formW;
    drift.set(0.7, sr); rateW.set(0.5, sr); loud.set(1.1, sr); formW.set(2.0, sr);
    music::Rng nrng(p.seed * 977 + static_cast<std::uint64_t>(v) * 31337);
    auto noise = [&]() { return nrng.uniform() * 2.0 - 1.0; };
    double breathHp = 0;                       // one-pole high-pass for the breath noise
    double pitchX = 0, pitchV = 0;             // the sung pitch (semitones, MIDI scale) and its velocity: a lightly under-damped follower of the written note
    double vibClock = 0;                       // seconds since vibrato started; kept across legato notes
    dsp::Biquad ring;                          // the singer's formant
    dsp::Biquad shelf;                         // higher-pole correction: the formants above F5 that a five-resonator model leaves out
    shelf.c = dsp::BiquadCoeffs::design(dsp::FilterType::HighShelf, sr, 2300.0, 16.0 * std::clamp(p.brightness, 0.2, 2.0), 0.6);
    double lastEnd = -10;                      // end of the previous vowel, for breaths at phrase starts
    std::unique_ptr<BankVoice> curBv, prevBv;  // recorded vowels (voice bank), when there is one

    // per-voice timing: the notes do not land exactly on the grid
    std::vector<NoteEv> ev = evs;
    for (std::size_t k = 0; k < ev.size(); ++k) {
      const double early = gauss(rng) * 0.010 * H;
      const double lenCut = rng.uniform() * 0.030 * H;
      ev[k].start = std::max(k ? ev[k - 1].start + 0.05 : 0.0, ev[k].start + early);
      ev[k].end = std::max(ev[k].start + 0.08, ev[k].end - lenCut + gauss(rng) * 0.008 * H);
      if (k) ev[k - 1].end = std::min(ev[k - 1].end, ev[k].start + 0.01);
    }

    for (std::size_t k = 0; k < ev.size(); ++k) {
      const NoteEv& e = ev[k];
      const Syllable& sy = syls[static_cast<std::size_t>(e.syl)];
      const std::string vowelName = vowels().count(sy.vowel) ? sy.vowel : "ah";
      const Formants& F = vowels().at(vowelName);
      const double target0 = e.pitch + detune / 100.0;
      const double f0 = 440.0 * std::pow(2.0, (target0 - 69.0) / 12.0);
      const bool legato = prevEnd >= 0 && e.start - prevEnd < 0.08 && prevF0 > 0;
      const double nextStart = k + 1 < ev.size() ? ev[k + 1].start : e.end + 1.0;
      const bool nextLegato = k + 1 < ev.size() && nextStart - e.end < 0.08;
      const double velN = std::clamp(e.vel / 127.0, 0.05, 1.0);
      // effort: louder = brighter and tighter; higher notes are also sung harder
      const double effort = std::clamp(0.18 + 0.62 * velN + 0.15 * (e.pitch - 60) / 24.0 + effortOffset + p.effortBias, 0.05, 1.0);
      {
        const BankEntry* be = p.bank ? p.bank->find(vowelName, f0) : nullptr;
        if (p.bank && !be) ++res.bankMissing;
        if (be) {
          prevBv = std::move(curBv); curBv = std::make_unique<BankVoice>(*be, sr);
          if (v == 0) { ++res.bankNotes; if (std::fabs(12.0 * std::log2(f0 / be->f0)) > 5.0) ++res.bankFarNotes; }
        } else { prevBv.reset(); curBv.reset(); }
      }
      const double tc = lerp(0.16, 0.055, effort), to = lerp(0.86, 0.66, effort) - tc;
      const double norm = 1.0 / sourceRms(to, tc);
      const double lpA = 1.0 - std::exp(-2.0 * M_PI * std::min(0.45 * sr, lerp(4500.0, 14000.0, effort)) / sr);

      // --- onset consonants, finishing at the note start
      if (!e.melisma) {
        double total2 = 0; std::vector<std::pair<std::string, Consonant>> cs;
        for (auto& c : consonantList(sy.onset)) { Consonant cc; if (consonant(c, cc)) { useRecording(p.bank, c, cc, res); cs.push_back({c, cc}); total2 += cc.dur; } }
        const double room = std::max(0.0, e.start - std::max(0.0, prevEnd) - 0.005);
        const double scale = total2 > room && total2 > 0 ? room / total2 : 1.0;
        double t = e.start - total2 * scale;
        for (auto& [name, cc] : cs) {
          const double d = cc.dur * scale;
          const std::size_t a = static_cast<std::size_t>(std::max(0.0, (t + delay)) * sr), b = std::min(total, static_cast<std::size_t>((t + delay + d) * sr));
          if (cc.rec) { playRecording(*cc.rec, out, a, b, sr, kBankLevel * lerp(0.55, 1.0, velN) * voiceGain); t += d; continue; }
          const Formants M = murmur(name);
          Res nz; if (cc.noiseHz > 0) nz.set(cc.noiseHz, cc.noiseBw, sr, false);
          for (int q = 0; q < 5; ++q) fr[q].set(M.f[q] * fscale, M.bw[q], sr);
          for (std::size_t i = a; i < b; ++i) {
            const double x = static_cast<double>(i - a) / std::max<std::size_t>(1, b - a);
            const double env = std::min({1.0, x * 8, (1 - x) * 8});
            double s = 0;
            if (cc.noiseAmp > 0) s += cc.noiseAmp * 3.0 * nz.run(noise());
            if (cc.voiced && cc.voiceAmp > 0) {
              double fl;
              double y = gl.next(rng, f0, sr, to, tc, norm * 1.9, lpA, fl);
              for (int q = 0; q < 5; ++q) y = fr[q].run(y);
              s += cc.voiceAmp * 4.0 * M.gain * y * kSource;
            }
            out[i] += static_cast<float>(s * env * 0.5);
          }
          t += d;
        }
      }

      // --- the vowel
      const double vStart = e.start;
      const double vEnd = std::max(e.start + 0.06, std::min(e.end, nextStart - 0.01));
      double codaDur = 0; std::vector<std::pair<std::string, Consonant>> codas;
      {
        const bool lastForSyl = k + 1 >= ev.size() || ev[k + 1].syl != e.syl || !ev[k + 1].melisma;
        if (lastForSyl) for (auto& c : consonantList(sy.coda)) { Consonant cc; if (consonant(c, cc)) { useRecording(p.bank, c, cc, res); codas.push_back({c, cc}); codaDur += cc.dur; } }
      }
      const double codaRoom = std::max(0.0, nextStart - vEnd - 0.02);
      const double vowelEnd = std::max(vStart + 0.05, vEnd);
      const std::size_t a = static_cast<std::size_t>((vStart + delay) * sr), b = std::min(total, static_cast<std::size_t>((vowelEnd + delay) * sr));
      const Formants P = !prevVowel.empty() && vowels().count(prevVowel) ? vowels().at(prevVowel) : F;
      const std::size_t glide = static_cast<std::size_t>(lerp(0.035, 0.07, H) * sr);
      const double attack = (legato ? 0.012 : 0.016 + 0.012 * H) * sr;
      const double relLen = std::min(0.07 + 0.03 * H, (vowelEnd - vStart) * 0.45);
      const bool phraseStart = vStart - lastEnd > 0.3;

      // pitch: a follower of the written note (natural glides and a little overshoot after leaps); a new note after a rest scoops up from below
      const double w = 2.0 * M_PI * lerp(11.0, 6.5, H), zeta = lerp(0.9, 0.68, H);
      if (!legato) {
        const double scoop = (0.45 + 0.6 * rng.uniform()) * H * (phraseStart ? 1.0 : 0.6);
        pitchX = target0 - scoop; pitchV = 0;
        vibClock = 0;
      } else if (pitchX == 0) pitchX = prevF0 > 0 ? 69.0 + 12.0 * std::log2(prevF0 / 440.0) : target0;
      const double vibDelay = 0.16 + 0.26 * rng.uniform();         // vibrato does not start with the note
      const double noteVib = (0.75 + 0.5 * rng.uniform()) * vibScale;
      const double droop = (0.10 + 0.25 * rng.uniform()) * H;       // the pitch sags a little as the note ends
      const double vibPhaseSlip = rng.uniform();
      (void)vibPhaseSlip;
      ring.c = dsp::BiquadCoeffs::design(dsp::FilterType::Peaking, sr, 2900.0 * fscale / vi.scale * (0.96 + 0.08 * rng.uniform()), p.singerFormantDb * (0.35 + 0.65 * effort), 1.3);
      ring.z1 = ring.z2 = 0;
      const double base = lerp(0.55, 1.0, velN);                    // loudness of this note

      for (std::size_t i = a; i < b; ++i) {
        const std::size_t n = i - a;
        const double tt = static_cast<double>(n) * dt;
        const double remaining = static_cast<double>(b - i) * dt;
        // pitch contour
        double tgt = target0 + drift.next(rng) * 0.055 * H;
        if (!nextLegato && remaining < 0.16) { const double q = 1.0 - remaining / 0.16; tgt -= droop * q * q; }
        const double acc = w * w * (tgt - pitchX) - 2.0 * zeta * w * pitchV;
        pitchV += acc * dt; pitchX += pitchV * dt;
        // vibrato: late, wandering in rate and depth, building up smoothly
        vibClock += dt;
        vibPhase += 2.0 * M_PI * vibRate * (1.0 + 0.05 * H * rateW.next(rng)) * dt;
        const double vdepth = smooth01((vibClock - vibDelay) / 0.35) * noteVib * (1.0 + 0.12 * H * std::sin(0.9 * vibClock * 2.0 * M_PI * 0.25));
        const double vib = (p.vibratoCents / 100.0) * vdepth * std::sin(vibPhase);
        const double f = 440.0 * std::pow(2.0, (pitchX + vib - 69.0) / 12.0);

        // formants: a smooth change from the previous vowel, with a tiny wobble
        if (n % 32 == 0) {
          const double mixv = legato || n < glide ? smooth01(static_cast<double>(n) / static_cast<double>(glide)) : 1.0;
          const double wob = 1.0 + 0.006 * H * formW.next(rng);
          for (int q = 0; q < 5; ++q) {
            double fq = (P.f[q] + (F.f[q] - P.f[q]) * mixv) * fscale * wob, bw = (P.bw[q] + (F.bw[q] - P.bw[q]) * mixv) * kUpperBw[q];
            if (q == 0 && f > fq * 0.85) fq = f * 1.08;      // high notes: the first formant follows the pitch
            fr[q].set(fq, bw, sr);
          }
        }
        // source, with pulsed breath noise (more when soft, and a puff when a phrase begins)
        double s = 0;
        if (curBv) {   // a real person's recording, moved to the sung pitch (it already has its own breath, formants and ring)
          const double wmix = smooth01(static_cast<double>(n) / static_cast<double>(glide));
          s = curBv->next(f);
          if (prevBv && (legato || n < glide) && wmix < 1.0) s = (1.0 - wmix) * prevBv->next(f) + wmix * s;
        } else {
          double flow;
          s = gl.next(rng, f, sr, to, tc, norm, lpA, flow);
          double bn = noise();
          breathHp += 0.4 * (bn - breathHp);
          const double puff = phraseStart ? 0.22 * std::exp(-tt / 0.05) : 0.0;
          s += (p.breath * (1.7 - effort) * (0.35 + 0.65 * flow) + puff) * (bn - breathHp) * 2.4;
          for (int q = 0; q < 5; ++q) s = fr[q].run(s);
          s = shelf.run(static_cast<float>(s));
          s = ring.run(static_cast<float>(s));
        }
        if (p.roughness > 0) {   // irregular ~65 Hz amplitude modulation: the roughness measured in screams and rasp
          roughPhase += 2.0 * M_PI * (58.0 + 14.0 * rateW.next(rng)) * dt;
          s *= 1.0 - 0.45 * p.roughness * (0.5 + 0.5 * std::sin(roughPhase)) * (0.7 + 0.3 * nrng.uniform());
        }
        if (p.fry > 0 && !nextLegato && remaining < 0.22) {   // vocal fry at the end of a phrase: sparse, irregular pulses
          frySince += dt;
          if (frySince > fryGap) { frySince = 0; fryGap = 0.022 + 0.03 * nrng.uniform(); }
          const double pulse = std::exp(-frySince / 0.006);
          const double q = 1.0 - remaining / 0.22;
          s *= 1.0 - p.fry * q * (1.0 - pulse);
        }
        // loudness: attack, slow swell, coupled with the vibrato, and a dip on a new syllable
        double env = std::min(1.0, static_cast<double>(n) / std::max(1.0, attack));
        if (remaining < relLen) env *= remaining / relLen;
        const double swell = 1.0 + 0.08 * H * loud.next(rng) + 0.07 * H * vdepth * std::sin(vibPhase + 0.6);
        double dip = 1.0;
        if (legato && !e.melisma) dip = 1.0 - 0.18 * std::exp(-tt / 0.04);
        out[i] += static_cast<float>(env * s * (curBv ? kBankLevel : kSource * F.gain) * base * swell * dip * voiceGain);
      }
      lastEnd = vowelEnd;
      // --- coda consonants after the vowel
      {
        double t = vowelEnd + 0.004;
        for (auto& [name, cc] : codas) {
          const double d = std::min(cc.dur, std::max(0.02, codaRoom * cc.dur / std::max(codaDur, 1e-3)));
          const std::size_t a2 = static_cast<std::size_t>((t + delay) * sr), b2 = std::min(total, static_cast<std::size_t>((t + delay + d) * sr));
          if (cc.rec) { playRecording(*cc.rec, out, a2, b2, sr, kBankLevel * lerp(0.55, 1.0, velN) * voiceGain); t += d; continue; }
          const Formants M = murmur(name);
          Res nz; if (cc.noiseHz > 0) nz.set(cc.noiseHz, cc.noiseBw, sr, false);
          for (int q = 0; q < 5; ++q) fr[q].set(M.f[q] * fscale, M.bw[q], sr);
          for (std::size_t i = a2; i < b2; ++i) {
            const double x = static_cast<double>(i - a2) / std::max<std::size_t>(1, b2 - a2);
            const double env = std::min({1.0, x * 8, (1 - x) * 4});
            double s = 0;
            if (cc.noiseAmp > 0) s += cc.noiseAmp * 3.0 * nz.run(noise());
            if (cc.voiced && cc.voiceAmp > 0) {
              double fl;
              double y = gl.next(rng, f0, sr, to, tc, norm * 1.9, lpA, fl);
              for (int q = 0; q < 5; ++q) y = fr[q].run(y);
              s += cc.voiceAmp * 4.0 * M.gain * y * kSource;
            }
            out[i] += static_cast<float>(s * env * 0.5);
          }
          t += d;
        }
        prevEnd = std::max(vowelEnd, t);
      }
      prevF0 = f0; prevVowel = vowelName;
    }
    const float g = 1.f / std::sqrt(static_cast<float>(nv));
    for (std::size_t i = 0; i < total; ++i) mix.channel(0)[i] += out[i] * g;
  }

  // a small room around the voice
  if (p.room > 0.0) {
    Room room(sr, std::clamp(p.room, 0.0, 1.0));
    const float wet = static_cast<float>(0.5 * std::clamp(p.room, 0.0, 1.0));
    for (std::size_t i = 0; i < total; ++i) { const float x = mix.channel(0)[i]; mix.channel(0)[i] = x + wet * room.run(x); }
  }
  float pk = 0; std::size_t bad = 0;
  for (std::size_t i = 0; i < total; ++i) { const float v = mix.channel(0)[i]; if (!std::isfinite(v)) { ++bad; mix.channel(0)[i] = 0.f; } else pk = std::max(pk, std::fabs(v)); }
  if (bad) std::fprintf(stderr, "singing: %zu non-finite samples of %zu\n", bad, total);
  if (pk < 1e-9f) { res.message = "Nothing audible was produced."; return res; }
  if (p.normalize) { const float norm = 0.6f / pk; for (std::size_t i = 0; i < total; ++i) mix.channel(0)[i] *= norm; }
  res.audio = std::move(mix);
  res.ok = true;
  res.message = "Sang " + std::to_string(res.usedSyllables) + " of " + std::to_string(res.syllables) + " syllables over " + std::to_string(res.notes) + " notes (" + voiceName(p.voice) + ").";
  if (res.usedSyllables < res.syllables) res.message += " The melody is shorter than the lyrics, so the last " + std::to_string(res.syllables - res.usedSyllables) + " syllables were left out.";
  if (res.notes > res.usedSyllables) res.message += " Extra notes hold the last vowel (melisma).";
  if (res.octaveShift) res.message += " The melody was moved " + std::to_string(std::abs(res.octaveShift)) + " octave(s) " + (res.octaveShift > 0 ? "up" : "down") + " to fit the voice.";
  return res;
}

}  // namespace wezt::voice
