#include "music/notation.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace wezt::music {
namespace {

const int kLetterPc[7] = {0, 2, 4, 5, 7, 9, 11};   // C D E F G A B
const char kLetterName[7] = {'C', 'D', 'E', 'F', 'G', 'A', 'B'};

// accidental of each letter in the key signature: +1 sharp, -1 flat
void signature(int fifths, int acc[7]) {
  for (int i = 0; i < 7; ++i) acc[i] = 0;
  static const int sharpOrder[7] = {3, 0, 4, 1, 5, 2, 6};   // F C G D A E B
  static const int flatOrder[7] = {6, 2, 5, 1, 4, 0, 3};    // B E A D G C F
  for (int i = 0; i < fifths && i < 7; ++i) acc[sharpOrder[i]] = 1;
  for (int i = 0; i < -fifths && i < 7; ++i) acc[flatOrder[i]] = -1;
}

int octaveOf(int midi, int letter, int acc) {
  // octave such that 12*(oct+1) + letterPc + acc == midi
  return (midi - kLetterPc[letter] - acc) / 12 - 1;
}

struct Spelled { int letter, acc, octave; };

Spelled spell(int midi, int fifths) {
  int sig[7]; signature(fifths, sig);
  const int pc = ((midi % 12) + 12) % 12;
  // 1. a note of the key signature itself
  for (int l = 0; l < 7; ++l) if (((kLetterPc[l] + sig[l]) % 12 + 12) % 12 == pc) return {l, sig[l], octaveOf(midi, l, sig[l])};
  // 2. a natural letter (printed with a natural sign when the key signature alters that letter)
  for (int l = 0; l < 7; ++l) if (kLetterPc[l] == pc) return {l, 0, octaveOf(midi, l, 0)};
  // 3. otherwise sharpen (in sharp keys) or flatten (in flat keys) a natural letter
  const int want = fifths >= 0 ? 1 : -1;
  for (int l = 0; l < 7; ++l) if (((kLetterPc[l] + want) % 12 + 12) % 12 == pc) return {l, want, octaveOf(midi, l, want)};
  return {0, 0, octaveOf(midi, 0, 0)};
}

struct Piece { int value, dots; double len; };
// durations in beats (quarter = 1): value index, dots, length, alignment unit (beats)
struct Dur { int value, dots; double len, unit; };
const Dur kDurs[] = {{0, 0, 4.0, 4.0}, {1, 1, 3.0, 4.0}, {1, 0, 2.0, 2.0}, {2, 1, 1.5, 1.0}, {2, 0, 1.0, 1.0}, {3, 1, 0.75, 0.5}, {3, 0, 0.5, 0.5}, {4, 0, 0.25, 0.25}};

bool aligned(double pos, double unit) { const double r = std::fmod(pos + 1e-9, unit); return r < 2e-9 || unit - r < 2e-9 ? true : r < 1e-6; }

std::vector<Piece> decompose(double pos, double len) {
  std::vector<Piece> out;
  int guard = 0;
  while (len > 1e-6 && guard++ < 64) {
    const Dur* pick = &kDurs[7];
    for (const Dur& d : kDurs) if (d.len <= len + 1e-9 && aligned(pos, d.unit) && (d.dots == 0 || aligned(pos + d.len, d.unit))) { pick = &d; break; }
    out.push_back({pick->value, pick->dots, pick->len});
    pos += pick->len; len -= pick->len;
  }
  return out;
}

}  // namespace

int keyFifths(const Key& key) {
  static const int majorFifths[12] = {0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5};
  int toMajor = 0;
  switch (key.mode) {
    case Mode::Major: toMajor = 0; break;
    case Mode::Minor: toMajor = 3; break;
    case Mode::Dorian: toMajor = -2; break;
    case Mode::Phrygian: toMajor = -4; break;
    case Mode::Lydian: toMajor = -5; break;
    case Mode::Mixolydian: toMajor = -7; break;
  }
  return majorFifths[(((key.root + toMajor) % 12) + 12) % 12];
}

std::string spellPitch(int midi, int fifths) {
  const Spelled s = spell(midi, fifths);
  std::string n(1, kLetterName[s.letter]);
  if (s.acc == 1) n += "#"; else if (s.acc == -1) n += "b";
  return n + std::to_string(octaveOf(midi, s.letter, s.acc));
}

int stepOf(int midi, int fifths) {
  const Spelled s = spell(midi, fifths);
  return 7 * octaveOf(midi, s.letter, s.acc) + s.letter;
}

int bottomLineStep(Clef c) { return c == Clef::Treble ? 7 * 4 + 2 : 7 * 2 + 4; }   // E4, G2

Score engrave(const MidiClip& clip, const Key& key, int bpb, int forceClef) {
  Score sc; sc.key = key; sc.fifths = keyFifths(key); sc.beatsPerBar = std::max(1, bpb);
  const double q = 0.25;
  auto qr = [&](double v) { return std::round(v / q) * q; };

  // clef
  std::vector<int> ps; for (const Note& n : clip.notes) ps.push_back(n.pitch);
  std::sort(ps.begin(), ps.end());
  sc.clef = forceClef == 0 ? Clef::Treble : forceClef == 1 ? Clef::Bass : (ps.empty() || ps[ps.size() / 2] >= 57 ? Clef::Treble : Clef::Bass);

  // chords: notes with the same rounded start
  struct Ev { double start, end; std::vector<int> pitches; };
  std::map<long long, Ev> byStart;
  for (const Note& n : clip.notes) {
    const double s = qr(n.start), e = std::max(s + q, qr(n.start + n.length));
    Ev& ev = byStart[std::llround(s / q)];
    if (ev.pitches.empty()) { ev.start = s; ev.end = e; } else ev.end = std::min(ev.end, e);
    ev.pitches.push_back(n.pitch);
  }
  std::vector<Ev> evs; for (auto& [k, e] : byStart) evs.push_back(e);
  for (std::size_t i = 0; i + 1 < evs.size(); ++i) evs[i].end = std::min(evs[i].end, evs[i + 1].start);

  const double barLen = sc.beatsPerBar;
  const double total = evs.empty() ? barLen : std::ceil((evs.back().end - 1e-9) / barLen) * barLen;
  const int nBars = std::max(1, static_cast<int>(std::lround(total / barLen)));
  sc.bars.resize(static_cast<std::size_t>(nBars));
  for (int b = 0; b < nBars; ++b) sc.bars[static_cast<std::size_t>(b)].number = b + 1;

  auto emit = [&](double start, double end, const std::vector<int>* pitches) {
    // cut at barlines, then at values
    double pos = start;
    while (pos < end - 1e-9) {
      const int bar = std::min(nBars - 1, static_cast<int>(std::floor((pos + 1e-9) / barLen)));
      const double barStart = bar * barLen, barEnd = barStart + barLen;
      const double segEnd = std::min(end, barEnd);
      const auto pieces = decompose(pos - barStart, segEnd - pos);
      double p = pos;
      for (std::size_t i = 0; i < pieces.size(); ++i) {
        ScoreEvent e; e.beat = p - barStart; e.length = pieces[i].len; e.value = pieces[i].value; e.dots = pieces[i].dots; e.rest = pitches == nullptr;
        if (pitches) {
          for (int m : *pitches) { ScoreNote n; n.midi = m; n.step = stepOf(m, sc.fifths); n.name = spellPitch(m, sc.fifths); e.notes.push_back(n); }
          e.tieToNext = (i + 1 < pieces.size()) || segEnd < end - 1e-9;
        }
        sc.bars[static_cast<std::size_t>(bar)].events.push_back(std::move(e));
        p += pieces[i].len;
      }
      pos = segEnd;
    }
  };

  double cursor = 0;
  for (const Ev& ev : evs) {
    if (ev.start > cursor + 1e-9) emit(cursor, ev.start, nullptr);
    std::vector<int> ps2 = ev.pitches; std::sort(ps2.begin(), ps2.end());
    emit(ev.start, ev.end, &ps2);
    cursor = ev.end;
  }
  if (cursor < total - 1e-9 && !evs.empty()) emit(cursor, total, nullptr);
  if (evs.empty()) emit(0, barLen, nullptr);

  // accidentals: print one where the pitch differs from the key signature, and not again in the same
  // bar for the same letter and octave
  int sig[7]; signature(sc.fifths, sig);
  for (ScoreBar& bar : sc.bars) {
    std::map<int, int> state;   // step -> accidental in force in this bar
    for (ScoreEvent& e : bar.events) for (ScoreNote& n : e.notes) {
      const Spelled s = spell(n.midi, sc.fifths);
      const int inForce = state.count(n.step) ? state[n.step] : sig[s.letter];
      if (s.acc != inForce) { n.accidental = s.acc; state[n.step] = s.acc; }
    }
  }
  return sc;
}

}  // namespace wezt::music
