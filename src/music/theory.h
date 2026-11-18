#pragma once
// Music theory and rule-based composition: scales, chords, progressions by style and mood, melodies,
// and session-player parts (bass, keys, pad, drums) that follow the chords.
//
// How this works, plainly: it is hand-written music theory (functional harmony, voice leading,
// chord-tone targeting) driven by a seeded random number generator, so the same inputs give the same
// result and every choice can be inspected. It does not contain a model trained on recordings.
// `ChordModel` can additionally learn transition counts from progressions you give it.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "music/midi.h"

namespace wezt::music {

enum class Mode { Major, Minor, Dorian, Phrygian, Lydian, Mixolydian };
const char* modeName(Mode m);
bool parseMode(const std::string& s, Mode& m);

struct Key {
  int root = 0;  // pitch class, 0 = C
  Mode mode = Mode::Major;
};
std::string noteName(int pitchClass);
bool parseKey(const std::string& text, Key& k);   // "C", "F# minor", "Bb dorian", "Am"
std::string keyName(const Key& k);
std::vector<int> scaleIntervals(Mode m);          // 7 semitone offsets
// Pitch class of scale degree d (0-based, may exceed 6).
int degreePitchClass(const Key& k, int degree);

struct Chord {
  int root = 0;                  // pitch class
  std::vector<int> intervals;    // from the root, ascending (0, 4, 7, 11 ...)
  int bass = -1;                 // pitch class of the bass note, -1 = root
  double beats = 4;
  int degree = 0;                // scale degree it was built on (0-based)
  std::string name() const;      // "Am7", "Fmaj7", "G"
};

// Builds the diatonic chord on a scale degree. extensions: 0 triad, 1 seventh, 2 ninth.
Chord diatonicChord(const Key& k, int degree, int extensions);

struct Mood { std::string name; };
struct StyleInfo {
  std::string name;
  int defaultBpm;
  Mode defaultMode;
};
// Styles: pop, rock, lofi, edm, jazz, cinematic, rnb, folk, trap. Moods: happy, sad, dreamy, dark,
// uplifting, tense, chill. Unknown names fall back to pop / happy.
std::vector<std::string> styleNames();
std::vector<std::string> moodNames();
StyleInfo styleInfo(const std::string& style);
Mode modeForMood(const std::string& mood, Mode fallback);
int extensionsFor(const std::string& style, const std::string& mood);

// Small deterministic generator (the standard distributions differ between platforms).
class Rng {
 public:
  explicit Rng(std::uint64_t seed = 1) : s_(seed * 2862933555777941757ull + 3037000493ull) { for (int i = 0; i < 4; ++i) next(); }
  std::uint64_t next() { s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17; return s_; }
  double uniform() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }
  int range(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
  bool chance(double p) { return uniform() < p; }
 private:
  std::uint64_t s_;
};

// Learns which degree follows which from progressions you supply (as degree lists, 0-based).
class ChordModel {
 public:
  void learn(const std::vector<int>& degrees);
  bool trained() const { return total_ > 0; }
  // Next degree given the previous one; falls back to `fallback` when unseen.
  int next(int prev, Rng& rng, int fallback) const;
  std::string save() const;
  bool load(const std::string& text);
 private:
  std::map<int, std::map<int, int>> counts_;
  int total_ = 0;
};

struct ProgressionParams {
  Key key;
  std::string style = "pop", mood = "happy";
  int bars = 8;
  double beatsPerChord = 4;      // 4 = one chord a bar; 2 = two a bar
  std::uint64_t seed = 1;
  const ChordModel* learned = nullptr;  // optional: blends in your own progressions
};
std::vector<Chord> generateProgression(const ProgressionParams& p);

struct MelodyParams {
  Key key;
  std::string style = "pop", mood = "happy";
  std::uint64_t seed = 1;
  int lowPitch = 62, highPitch = 81;
  double density = 0.5;          // 0 sparse .. 1 busy
  int beatsPerBar = 4;
};
// Follows the chords: strong beats land on chord tones, phrases repeat and vary, and the last note of
// a phrase resolves.
MidiClip generateMelody(const std::vector<Chord>& chords, const MelodyParams& p);

struct PartParams {
  Key key;
  std::string style = "pop";
  std::uint64_t seed = 1;
  int beatsPerBar = 4;
  double energy = 0.6;           // 0 calm .. 1 driving
};
MidiClip bassPart(const std::vector<Chord>& chords, const PartParams& p);
MidiClip keysPart(const std::vector<Chord>& chords, const PartParams& p);   // comping or arpeggio, by style
MidiClip padPart(const std::vector<Chord>& chords, const PartParams& p);    // smooth voice-led sustained chords
MidiClip drumPart(int bars, const PartParams& p, bool fillAtEnd = true);    // General MIDI drum map, channel 9

// General MIDI drum numbers used by drumPart.
enum Drum { Kick = 36, Snare = 38, Clap = 39, ClosedHat = 42, OpenHat = 46, Tom = 45, Crash = 49, Ride = 51 };

}  // namespace wezt::music
