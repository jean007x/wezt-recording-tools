#include "music/theory.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace wezt::music {
namespace {

const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
int mod12(int x) { return ((x % 12) + 12) % 12; }

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

bool minorish(Mode m) { return m == Mode::Minor || m == Mode::Dorian || m == Mode::Phrygian; }

template <class T> const T& pick(const std::vector<T>& v, Rng& r) { return v[static_cast<std::size_t>(r.range(static_cast<int>(v.size())))]; }

// Scale pitches (ascending) within [lo, hi].
std::vector<int> scalePitches(const Key& k, int lo, int hi) {
  std::vector<int> out;
  const auto iv = scaleIntervals(k.mode);
  for (int p = lo; p <= hi; ++p) {
    const int rel = mod12(p - k.root);
    if (std::find(iv.begin(), iv.end(), rel) != iv.end()) out.push_back(p);
  }
  return out;
}

const Chord* chordAt(const std::vector<Chord>& cs, double beat, double* chordStart = nullptr) {
  double t = 0;
  for (auto& c : cs) {
    if (beat < t + c.beats - 1e-9) { if (chordStart) *chordStart = t; return &c; }
    t += c.beats;
  }
  if (chordStart) *chordStart = t;
  return cs.empty() ? nullptr : &cs.back();
}
double totalBeats(const std::vector<Chord>& cs) { double t = 0; for (auto& c : cs) t += c.beats; return t; }
bool isChordTone(const Chord& c, int pitch) {
  const int pc = mod12(pitch - c.root);
  for (int i : c.intervals) if (mod12(i) == pc) return true;
  return false;
}

}  // namespace

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Major: return "major"; case Mode::Minor: return "minor"; case Mode::Dorian: return "dorian";
    case Mode::Phrygian: return "phrygian"; case Mode::Lydian: return "lydian"; case Mode::Mixolydian: return "mixolydian";
  }
  return "major";
}
bool parseMode(const std::string& s0, Mode& m) {
  const std::string s = lower(s0);
  if (s == "major" || s == "maj" || s == "ionian") m = Mode::Major;
  else if (s == "minor" || s == "min" || s == "m" || s == "aeolian") m = Mode::Minor;
  else if (s == "dorian") m = Mode::Dorian;
  else if (s == "phrygian") m = Mode::Phrygian;
  else if (s == "lydian") m = Mode::Lydian;
  else if (s == "mixolydian") m = Mode::Mixolydian;
  else return false;
  return true;
}
std::string noteName(int pc) { return kNames[mod12(pc)]; }

bool parseKey(const std::string& text, Key& k) {
  std::size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i >= text.size()) return false;
  const char L = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // A..G
  if (L < 'A' || L > 'G') return false;
  int pc = base[L - 'A'];
  ++i;
  if (i < text.size() && (text[i] == '#' || text[i] == 'b') ) {
    // "b" after a letter is a flat, except in "Bb"-less words like "dorian"; letters are always followed by acc first
    pc += text[i] == '#' ? 1 : -1;
    ++i;
  }
  std::string rest = text.substr(i);
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
  Mode m = Mode::Major;
  if (!rest.empty() && !parseMode(rest, m)) return false;
  k.root = mod12(pc);
  k.mode = m;
  return true;
}
std::string keyName(const Key& k) { return noteName(k.root) + " " + modeName(k.mode); }

std::vector<int> scaleIntervals(Mode m) {
  switch (m) {
    case Mode::Major: return {0, 2, 4, 5, 7, 9, 11};
    case Mode::Minor: return {0, 2, 3, 5, 7, 8, 10};
    case Mode::Dorian: return {0, 2, 3, 5, 7, 9, 10};
    case Mode::Phrygian: return {0, 1, 3, 5, 7, 8, 10};
    case Mode::Lydian: return {0, 2, 4, 6, 7, 9, 11};
    case Mode::Mixolydian: return {0, 2, 4, 5, 7, 9, 10};
  }
  return {0, 2, 4, 5, 7, 9, 11};
}
int degreePitchClass(const Key& k, int d) {
  const auto iv = scaleIntervals(k.mode);
  const int idx = ((d % 7) + 7) % 7;
  return mod12(k.root + iv[static_cast<std::size_t>(idx)]);
}

std::string Chord::name() const {
  std::string s = noteName(root);
  auto has = [&](int x) { return std::find(intervals.begin(), intervals.end(), x) != intervals.end(); };
  const bool min3 = has(3), maj3 = has(4);
  const bool dim5 = has(6), aug5 = has(8);
  const bool m7 = has(10), M7 = has(11), d7 = has(9) && dim5 && min3;
  const bool n9 = has(14), n11 = has(17);
  if (min3 && dim5) s += (m7 ? "m7b5" : d7 ? "dim7" : "dim");
  else if (aug5 && maj3) s += "aug";
  else {
    if (min3) s += "m";
    if (M7) s += min3 ? "(maj7)" : "maj7";
    else if (m7) s += n9 ? "9" : "7";
    else if (n9) s += "add9";
    if (M7 && n9) s += "9";
  }
  if (n11 && !n9) s += "add11";
  if (bass >= 0 && mod12(bass) != mod12(root)) s += "/" + noteName(bass);
  return s;
}

Chord diatonicChord(const Key& k, int degree, int ext) {
  Chord c;
  c.degree = ((degree % 7) + 7) % 7;
  c.root = degreePitchClass(k, degree);
  static const int steps[5] = {0, 2, 4, 6, 8};
  const int count = 3 + std::clamp(ext, 0, 2);
  int prev = -1;
  for (int i = 0; i < count; ++i) {
    int iv = mod12(degreePitchClass(k, degree + steps[i]) - c.root);
    while (iv <= prev) iv += 12;
    c.intervals.push_back(iv);
    prev = iv;
  }
  return c;
}

std::vector<std::string> styleNames() { return {"pop", "rock", "lofi", "edm", "jazz", "cinematic", "rnb", "folk", "trap"}; }
std::vector<std::string> moodNames() { return {"happy", "sad", "dreamy", "dark", "uplifting", "tense", "chill"}; }

StyleInfo styleInfo(const std::string& style0) {
  const std::string s = lower(style0);
  if (s == "rock") return {"rock", 124, Mode::Major};
  if (s == "lofi") return {"lofi", 78, Mode::Dorian};
  if (s == "edm") return {"edm", 128, Mode::Minor};
  if (s == "jazz") return {"jazz", 110, Mode::Major};
  if (s == "cinematic") return {"cinematic", 90, Mode::Minor};
  if (s == "rnb") return {"rnb", 88, Mode::Minor};
  if (s == "folk") return {"folk", 104, Mode::Major};
  if (s == "trap") return {"trap", 140, Mode::Minor};
  return {"pop", 110, Mode::Major};
}
Mode modeForMood(const std::string& mood0, Mode fallback) {
  const std::string m = lower(mood0);
  if (m == "happy" || m == "uplifting") return Mode::Major;
  if (m == "sad" || m == "dark") return Mode::Minor;
  if (m == "dreamy") return Mode::Lydian;
  if (m == "tense") return Mode::Phrygian;
  if (m == "chill") return Mode::Dorian;
  return fallback;
}
int extensionsFor(const std::string& style0, const std::string& mood0) {
  const std::string s = lower(style0), m = lower(mood0);
  int e = 0;
  if (s == "jazz") e = 2; else if (s == "lofi" || s == "rnb") e = 1; else if (s == "cinematic" || s == "trap") e = 0;
  if (m == "dreamy" || m == "chill") e += 1;
  if (m == "tense" && e < 1) e = 1;
  return std::clamp(e, 0, 2);
}

void ChordModel::learn(const std::vector<int>& d) {
  for (std::size_t i = 0; i + 1 < d.size(); ++i) { ++counts_[d[i]][d[i + 1]]; ++total_; }
}
int ChordModel::next(int prev, Rng& rng, int fallback) const {
  auto it = counts_.find(prev);
  if (it == counts_.end()) return fallback;
  int sum = 0;
  for (auto& [k, v] : it->second) sum += v;
  int r = rng.range(sum);
  for (auto& [k, v] : it->second) { if (r < v) return k; r -= v; }
  return fallback;
}
std::string ChordModel::save() const {
  std::ostringstream o;
  for (auto& [a, m] : counts_) for (auto& [b, n] : m) o << a << ' ' << b << ' ' << n << '\n';
  return o.str();
}
bool ChordModel::load(const std::string& text) {
  counts_.clear(); total_ = 0;
  std::istringstream in(text);
  int a, b, n;
  while (in >> a >> b >> n) { if (a < 0 || a > 6 || b < 0 || b > 6 || n <= 0) return false; counts_[a][b] += n; total_ += n; }
  return true;
}

std::vector<Chord> generateProgression(const ProgressionParams& p) {
  Rng rng(p.seed);
  const std::string style = lower(p.style);
  const bool minor = minorish(p.key.mode);
  const int ext = extensionsFor(p.style, p.mood);
  using T = std::vector<int>;
  std::vector<T> pool;
  if (!minor) {
    pool = {{0, 4, 5, 3}, {0, 5, 3, 4}, {0, 3, 4, 3}, {0, 2, 3, 4}, {0, 3, 0, 4}, {0, 5, 1, 4}};
    if (style == "jazz") pool = {{1, 4, 0, 5}, {0, 5, 1, 4}, {2, 5, 1, 4}, {0, 2, 1, 4}};
    if (style == "rnb" || style == "lofi") pool = {{1, 4, 0, 5}, {0, 2, 3, 4}, {3, 2, 1, 0}, {0, 5, 1, 4}};
    if (style == "edm") pool = {{0, 4, 5, 3}, {5, 3, 0, 4}, {0, 5, 3, 4}};
    if (style == "folk") pool = {{0, 3, 0, 4}, {0, 4, 3, 4}, {0, 5, 3, 4}};
    if (style == "cinematic") pool = {{0, 5, 3, 4}, {0, 2, 5, 3}, {0, 3, 5, 4}};
  } else {
    pool = {{0, 5, 6, 0}, {0, 6, 5, 6}, {0, 3, 6, 2}, {0, 5, 2, 6}, {0, 2, 6, 3}};
    if (style == "jazz") pool = {{1, 4, 0, 0}, {0, 3, 6, 2}, {0, 5, 1, 4}};
    if (style == "rnb" || style == "lofi") pool = {{0, 3, 6, 2}, {0, 5, 2, 6}, {3, 6, 0, 0}, {0, 6, 3, 4}};
    if (style == "edm" || style == "trap") pool = {{0, 5, 6, 0}, {0, 0, 5, 6}, {0, 3, 5, 6}, {0, 6, 5, 4}};
    if (style == "cinematic") pool = {{0, 5, 2, 6}, {0, 6, 5, 4}, {0, 2, 5, 6}};
  }
  const T& tmpl = pick(pool, rng);
  const int total = std::max(1, static_cast<int>(std::lround(p.bars * 4.0 / std::max(0.5, p.beatsPerChord))));
  std::vector<int> degrees;
  for (int i = 0; i < total; ++i) degrees.push_back(tmpl[static_cast<std::size_t>(i % static_cast<int>(tmpl.size()))]);
  // Light variation in the second pass of a template, so an 8-bar progression is not a copy of 4 bars.
  for (int i = static_cast<int>(tmpl.size()); i < total; ++i)
    if (rng.chance(0.25)) { int& d = degrees[static_cast<std::size_t>(i)]; if (d == 3) d = 1; else if (d == 5) d = 2; else if (d == 4 && !minor) d = 6 == 0 ? 4 : 4; }
  // Blend in the user's own progressions when a model was supplied.
  if (p.learned && p.learned->trained())
    for (int i = 1; i < total - 1; ++i)
      if (rng.chance(0.5)) degrees[static_cast<std::size_t>(i)] = p.learned->next(degrees[static_cast<std::size_t>(i - 1)], rng, degrees[static_cast<std::size_t>(i)]);
  // Cadence: dominant (or subtonic in modal/minor keys) into the tonic.
  if (total >= 4) {
    degrees[static_cast<std::size_t>(total - 1)] = 0;
    const int pen = degrees[static_cast<std::size_t>(total - 2)];
    if (pen != 4 && pen != 6 && pen != 3) degrees[static_cast<std::size_t>(total - 2)] = (minor && p.key.mode != Mode::Dorian) ? 6 : 4;
  }
  degrees[0] = 0;
  std::vector<Chord> out;
  for (int d : degrees) {
    Chord c = diatonicChord(p.key, d, ext);
    c.beats = p.beatsPerChord;
    out.push_back(std::move(c));
  }
  return out;
}

MidiClip generateMelody(const std::vector<Chord>& chords, const MelodyParams& p) {
  MidiClip clip;
  clip.name = "Melody";
  if (chords.empty()) return clip;
  Rng rng(p.seed * 7 + 11);
  const double tb = totalBeats(chords);
  clip.lengthBeats = tb;
  const auto scale = scalePitches(p.key, p.lowPitch, p.highPitch);
  if (scale.size() < 4) return clip;
  const int bpb = std::max(2, p.beatsPerBar);
  const int bars = static_cast<int>(std::ceil(tb / bpb));

  struct R { double at, len; };
  using Rhythm = std::vector<R>;
  const std::vector<Rhythm> sparse = {{{0, 2}, {2, 2}}, {{0, 1.5}, {1.5, 0.5}, {2, 2}}, {{0, 1}, {1, 1}, {2, 2}}, {{0.5, 1.5}, {2, 2}}};
  const std::vector<Rhythm> medium = {{{0, 1}, {1, 0.5}, {1.5, 0.5}, {2, 1}, {3, 1}}, {{0, 0.5}, {0.5, 0.5}, {1, 1}, {2, 1}, {3, 0.5}, {3.5, 0.5}},
                                      {{0, 1.5}, {1.5, 0.5}, {2, 1}, {3, 1}}, {{0, 1}, {1.5, 0.5}, {2, 1.5}, {3.5, 0.5}}};
  const std::vector<Rhythm> busy = {{{0, 0.5}, {0.5, 0.5}, {1, 0.5}, {1.5, 0.5}, {2, 0.5}, {2.5, 0.5}, {3, 1}}, {{0, 0.5}, {0.5, 0.5}, {1, 1}, {2, 0.5}, {2.5, 0.5}, {3, 0.5}, {3.5, 0.5}},
                                    {{0, 0.75}, {0.75, 0.75}, {1.5, 0.5}, {2, 0.5}, {2.5, 0.5}, {3, 0.5}, {3.5, 0.5}}};
  auto pickRhythm = [&]() -> Rhythm {
    const double r = rng.uniform();
    if (r < 1.0 - p.density - 0.15) return pick(sparse, rng);
    if (r < 1.0 - p.density * 0.5) return pick(medium, rng);
    return pick(busy, rng);
  };
  // Phrase plan for the bars: A A' B A''. A rhythm repeats where the phrase repeats.
  std::vector<Rhythm> rhA = {pickRhythm(), pickRhythm()}, rhB = {pickRhythm(), pickRhythm()};
  const bool minorKey = minorish(p.key.mode);
  (void)minorKey;

  // Start near the middle of the range on a chord tone.
  auto nearestChordTone = [&](int idx, const Chord& c, int maxDist) {
    for (int d = 0; d <= maxDist; ++d)
      for (int s : {d, -d}) {
        const int j = idx + s;
        if (j >= 0 && j < static_cast<int>(scale.size()) && isChordTone(c, scale[static_cast<std::size_t>(j)])) return j;
      }
    return std::clamp(idx, 0, static_cast<int>(scale.size()) - 1);
  };
  int idx = nearestChordTone(static_cast<int>(scale.size()) / 2, chords[0], 4);
  int dir = rng.chance(0.5) ? 1 : -1;
  for (int bar = 0; bar < bars; ++bar) {
    const int phrase = (bar / 2) % 4;              // 0 A, 1 A', 2 B, 3 A''
    const bool second = bar % 2 == 1;
    const Rhythm& rh = (phrase == 2 ? rhB : rhA)[second ? 1 : 0];
    for (std::size_t k = 0; k < rh.size(); ++k) {
      const double beat = bar * bpb + rh[k].at;
      if (beat >= tb - 1e-9) break;
      const bool lastOfPhrase = second && k + 1 == rh.size();
      const bool lastOfSong = beat + rh[k].len >= tb - 1e-9 || (bar == bars - 1 && k + 1 == rh.size());
      // occasional rests keep it breathing, never on strong beats or at the end
      if (k > 0 && !lastOfPhrase && rh[k].at != std::floor(rh[k].at) && rng.chance((1.0 - p.density) * 0.25)) continue;
      const Chord& c = *chordAt(chords, beat);
      const bool strong = std::fmod(rh[k].at, 2.0) == 0.0;
      if (lastOfSong) {
        // land on the tonic (or the chord's root), held to the end
        int target = idx;
        int best = 99;
        for (int j = 0; j < static_cast<int>(scale.size()); ++j)
          if (mod12(scale[static_cast<std::size_t>(j)] - p.key.root) == 0 && std::abs(j - idx) < best) { best = std::abs(j - idx); target = j; }
        idx = target;
      } else if (lastOfPhrase) {
        idx = nearestChordTone(idx + dir, c, 3);
      } else {
        const double r = rng.uniform();
        int step = r < 0.45 ? dir : r < 0.65 ? -dir : r < 0.78 ? 0 : r < 0.90 ? 2 * dir : -2 * dir;
        if (rng.chance(0.08)) step = 3 * dir;  // an occasional leap
        int cand = std::clamp(idx + step, 0, static_cast<int>(scale.size()) - 1);
        idx = strong ? nearestChordTone(cand, c, 2) : cand;
      }
      if (idx <= 1) dir = 1; else if (idx >= static_cast<int>(scale.size()) - 2) dir = -1; else if (rng.chance(0.2)) dir = -dir;
      Note n;
      n.pitch = scale[static_cast<std::size_t>(idx)];
      n.start = beat;
      n.length = lastOfSong ? std::max(rh[k].len, tb - beat) : rh[k].len * 0.95;
      n.length = std::min(n.length, tb - beat);
      n.velocity = std::clamp((strong ? 98 : 84) + rng.range(11) - 5, 1, 127);
      clip.notes.push_back(n);
    }
  }
  return clip;
}

namespace {

// Voicing of a chord around `center`, chosen among inversions to move as little as possible from the
// previous voicing.
std::vector<int> voice(const Chord& c, const std::vector<int>& prev, int center, bool dropRoot = false) {
  std::vector<int> pcs;
  for (int i : c.intervals) pcs.push_back(mod12(c.root + i));
  if (dropRoot && pcs.size() > 3) pcs.erase(pcs.begin());
  std::vector<int> best;
  int bestCost = 1 << 30;
  for (std::size_t inv = 0; inv < pcs.size(); ++inv) {
    std::vector<int> v;
    int last = center - 8;
    for (std::size_t j = 0; j < pcs.size(); ++j) {
      int pc = pcs[(inv + j) % pcs.size()];
      int p = last - mod12(last - pc);
      while (p <= last) p += 12;
      if (j == 0) { p = center - 8; while (mod12(p) != pc) ++p; }
      v.push_back(p);
      last = p;
    }
    int cost = 0;
    if (prev.empty()) { int mean = 0; for (int x : v) mean += x; cost = std::abs(mean / static_cast<int>(v.size()) - center); }
    else { int a = 0, b = 0; for (int x : v) a += x; for (int x : prev) b += x; cost = std::abs(a / static_cast<int>(v.size()) - b / static_cast<int>(prev.size())); }
    if (cost < bestCost) { bestCost = cost; best = v; }
  }
  return best;
}

}  // namespace

MidiClip bassPart(const std::vector<Chord>& chords, const PartParams& p) {
  MidiClip clip; clip.name = "Bass";
  Rng rng(p.seed * 13 + 3);
  const std::string st = lower(p.style);
  clip.lengthBeats = totalBeats(chords);
  auto lowRoot = [](int pc) { int x = 24 + pc; while (x < 28) x += 12; while (x > 40) x -= 12; return x; };
  double t = 0;
  for (const Chord& c : chords) {
    const int root = lowRoot(c.root);
    const int fifth = root + 7;
    int third = root + (std::find(c.intervals.begin(), c.intervals.end(), 3) != c.intervals.end() ? 3 : 4);
    auto add = [&](double at, double len, int pitch, int vel) {
      if (at >= c.beats - 1e-9) return;
      clip.notes.push_back({pitch, t + at, std::min(len, c.beats - at) * 0.95, std::clamp(vel + rng.range(9) - 4, 1, 127)});
    };
    const double bars = c.beats / 4.0;
    for (int b = 0; b < std::max(1, static_cast<int>(std::lround(bars))); ++b) {
      const double o = b * 4.0;
      if (c.beats < 4) { add(0, c.beats, root, 100); break; }
      if (st == "edm") { for (int i = 0; i < 4; ++i) add(o + i + 0.5, 0.5, root, 96); }
      else if (st == "trap") { add(o, 2.5, root, 110); if (rng.chance(0.5)) add(o + 2.5, 1.0, root, 100); else add(o + 3, 1.0, fifth - 12 + 12, 95); }
      else if (st == "lofi" || st == "rnb" || st == "jazz") {
        add(o, 1.5, root, 100); add(o + 2.5, 0.5, third, 85);
        add(o + 3, 1.0, rng.chance(0.5) ? fifth : root + 12 > 43 ? root : root + 12, 88);
      }
      else if (st == "folk") { add(o, 1, root, 95); add(o + 1, 1, fifth, 80); add(o + 2, 1, root, 90); add(o + 3, 1, fifth, 80); }
      else if (st == "cinematic") { add(o, 4, root, 100); }
      else {  // pop, rock: driving eighths on the root with a fifth and an octave for movement
        for (int i = 0; i < 8; ++i) {
          int pitch = root;
          if (i == 3 && p.energy > 0.5) pitch = fifth;
          if (i == 6) pitch = (p.energy > 0.5 && rng.chance(0.5)) ? root + 12 > 43 ? root : root + 12 : root;
          add(o + i * 0.5, 0.5, pitch, i % 2 == 0 ? 100 : 82);
        }
      }
    }
    t += c.beats;
  }
  return clip;
}

MidiClip keysPart(const std::vector<Chord>& chords, const PartParams& p) {
  MidiClip clip; clip.name = "Keys";
  Rng rng(p.seed * 17 + 5);
  const std::string st = lower(p.style);
  clip.lengthBeats = totalBeats(chords);
  std::vector<int> prev;
  double t = 0;
  for (const Chord& c : chords) {
    const auto v = voice(c, prev, 60, st == "jazz" || st == "rnb");
    prev = v;
    auto stab = [&](double at, double len, int vel) {
      if (at >= c.beats - 1e-9) return;
      for (int pch : v) clip.notes.push_back({pch, t + at, std::min(len, c.beats - at) * 0.9, std::clamp(vel + rng.range(9) - 4, 1, 127)});
    };
    for (double o = 0; o < c.beats - 1e-9; o += 4.0) {
      if (st == "folk" || st == "cinematic") {  // rolling arpeggio, eighth notes
        for (int i = 0; i < 8; ++i) {
          const int pch = v[static_cast<std::size_t>((i < 4 ? i : 7 - i) % static_cast<int>(v.size()))];
          if (o + i * 0.5 < c.beats) clip.notes.push_back({pch, t + o + i * 0.5, 0.55, std::clamp(78 + rng.range(9) - 4, 1, 127)});
        }
      } else if (st == "edm") { stab(o, 0.5, 100); stab(o + 1.5, 0.5, 90); stab(o + 2.5, 0.5, 95); stab(o + 3.5, 0.5, 85); }
      else if (st == "lofi" || st == "rnb" || st == "jazz") { stab(o, 1.5, 88); stab(o + 2.5, 1.0, 80); }
      else if (st == "trap") { stab(o, 3.5, 85); }
      else { stab(o, 1.0, 92); stab(o + 1.5, 0.5, 80); stab(o + 2, 1.0, 88); stab(o + 3.5, 0.5, 78); }  // pop, rock
    }
    t += c.beats;
  }
  return clip;
}

MidiClip padPart(const std::vector<Chord>& chords, const PartParams& p) {
  MidiClip clip; clip.name = "Pad";
  Rng rng(p.seed * 19 + 1);
  clip.lengthBeats = totalBeats(chords);
  std::vector<int> prev;
  double t = 0;
  for (const Chord& c : chords) {
    const auto v = voice(c, prev, 64);
    prev = v;
    for (int pch : v) clip.notes.push_back({pch, t, c.beats * 1.0, std::clamp(70 + rng.range(9) - 4, 1, 127)});
    t += c.beats;
  }
  return clip;
}

MidiClip drumPart(int bars, const PartParams& p, bool fill) {
  MidiClip clip; clip.name = "Drums"; clip.channel = 9;
  Rng rng(p.seed * 23 + 9);
  const std::string st = lower(p.style);
  clip.lengthBeats = bars * p.beatsPerBar;
  const double e = std::clamp(p.energy, 0.0, 1.0);
  auto hit = [&](int drum, double beat, int vel) { clip.notes.push_back({drum, beat, 0.25, std::clamp(vel + rng.range(9) - 4, 1, 127)}); };
  const double step = 0.25;
  for (int bar = 0; bar < bars; ++bar) {
    const double o = bar * p.beatsPerBar;
    auto onSteps = [&](int drum, std::initializer_list<int> steps, int vel) { for (int s : steps) hit(drum, o + s * step, vel); };
    const bool lastBar = (bar % 4 == 3) || bar == bars - 1;
    if (st == "edm") {
      onSteps(Kick, {0, 4, 8, 12}, 110); onSteps(Clap, {4, 12}, 100); onSteps(OpenHat, {2, 6, 10, 14}, 80);
      if (e > 0.4) onSteps(ClosedHat, {1, 3, 5, 7, 9, 11, 13, 15}, 55);
    } else if (st == "trap") {
      onSteps(Kick, {0, 10}, 112); onSteps(Snare, {8}, 105); onSteps(Clap, {8}, 90);
      for (int s = 0; s < 16; ++s) hit(ClosedHat, o + s * step, s % 4 == 0 ? 85 : 60);
      if (rng.chance(0.4)) { hit(ClosedHat, o + 13.5 * step, 55); hit(ClosedHat, o + 14 * step, 55); hit(ClosedHat, o + 14.5 * step, 55); }
    } else if (st == "lofi") {
      onSteps(Kick, {0, 7, 10}, 95); onSteps(Snare, {4, 12}, 80);
      if (rng.chance(0.5)) hit(Snare, o + 15 * step, 40);  // ghost
      for (int s = 0; s < 16; s += 2) hit(ClosedHat, o + s * step + (s % 4 == 2 ? 0.08 : 0.0), s % 4 == 0 ? 70 : 52);  // swung eighths
    } else if (st == "jazz") {
      onSteps(Ride, {0, 4, 6, 8, 12, 14}, 78); onSteps(Kick, {0}, 60); if (rng.chance(0.5)) hit(Kick, o + 10 * step, 50);
      onSteps(ClosedHat, {4, 12}, 60);
      if (rng.chance(0.5)) hit(Snare, o + 7 * step, 45);
    } else if (st == "cinematic") {
      onSteps(Kick, {0, 8}, 100); if (bar % 2 == 0) hit(Tom, o + 12 * step, 90);
      if (bar % 4 == 0) hit(Crash, o, 95);
      if (e > 0.5) onSteps(Tom, {4, 6}, 80);
    } else if (st == "rnb") {
      onSteps(Kick, {0, 3, 10}, 100); onSteps(Snare, {4, 12}, 92);
      for (int s = 0; s < 16; s += 2) hit(ClosedHat, o + s * step, s % 4 == 0 ? 72 : 55);
      if (rng.chance(0.4)) hit(ClosedHat, o + 15 * step, 50);
    } else if (st == "folk") {
      onSteps(Kick, {0, 8}, 80); onSteps(Snare, {4, 12}, 70);
      for (int s = 0; s < 16; s += 4) hit(ClosedHat, o + s * step, 55);
    } else if (st == "rock") {
      onSteps(Kick, {0, 8, 10}, 108); onSteps(Snare, {4, 12}, 106);
      for (int s = 0; s < 16; s += 2) hit(ClosedHat, o + s * step, s % 4 == 0 ? 90 : 72);
      if (bar % 4 == 0) hit(Crash, o, 100);
    } else {  // pop
      onSteps(Kick, {0, 8}, 104); if (rng.chance(0.6)) hit(Kick, o + 10 * step, 90);
      onSteps(Snare, {4, 12}, 100);
      for (int s = 0; s < 16; s += 2) hit(ClosedHat, o + s * step, s % 4 == 0 ? 80 : 62);
      if (e > 0.7) onSteps(ClosedHat, {3, 11}, 50);
    }
    if (fill && lastBar && bars >= 4 && st != "jazz") {
      // a short snare/tom fill in the last beat replaces the closing hats
      auto& v = clip.notes;
      v.erase(std::remove_if(v.begin(), v.end(), [&](const Note& n) { return n.start >= o + 3.0 - 1e-9 && n.start < o + 4.0 - 1e-9 && n.pitch != Kick; }), v.end());
      for (int s = 12; s < 16; ++s) hit((s % 2) ? Tom : Snare, o + s * step, 80 + (s - 12) * 8);
    }
  }
  std::sort(clip.notes.begin(), clip.notes.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  return clip;
}

}  // namespace wezt::music
