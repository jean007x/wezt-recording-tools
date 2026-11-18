#pragma once
// A whole song, composed by rule: sections (intro, verse, chorus ...), a chord progression per
// section, and session-player parts that follow the chords. It can be rendered with the built-in
// instruments, written as a MIDI file plus stems, and opened in Wezt as a normal session.
#include <string>
#include <vector>

#include "engine/audio_buffer.h"
#include "music/synth.h"
#include "music/theory.h"

namespace wezt::music {

struct SectionSpec {
  std::string name = "verse";
  std::string kind = "verse";     // intro, verse, chorus, bridge, outro: decides the parts and the energy
  int bars = 8;
  double energy = -1;             // -1 = default for the kind
};

struct SongSpec {
  std::string title = "Untitled";
  Key key;
  bool keyGiven = false;          // false: the mode follows the mood
  std::string style = "pop", mood = "happy";
  double bpm = 0;                 // 0 = the style's usual tempo
  std::uint64_t seed = 1;
  std::vector<SectionSpec> sections;      // empty = intro 4, verse 8, chorus 8, verse 8, chorus 8, outro 4
  std::vector<std::string> parts;         // subset of drums bass keys pad melody; empty = all
};

struct SongTrack {
  std::string name;               // "Drums", "Bass", "Keys", "Pad", "Melody"
  std::string preset;             // synth preset, or "drums"
  MidiClip midi;                  // the whole song, in beats from the start
  float gainDb = 0;
  float pan = 0;
};
struct Placed { std::string name, kind; double startBeat = 0; int bars = 0; };

struct Song {
  SongSpec spec;
  Key key;
  double bpm = 110;
  double beats = 0;
  std::vector<Placed> layout;
  std::vector<Chord> chords;      // the whole song, one entry per chord
  std::vector<SongTrack> tracks;
  double seconds() const { return beats * 60.0 / bpm; }
  std::string chartText() const;  // sections with their chord names, for people (and for Claude) to read
};

Song composeSong(SongSpec spec);

struct Stem { std::string name; AudioBuffer audio; float gainDb = 0, pan = 0; };
std::vector<Stem> renderStems(const Song& song, double sampleRate);
// Sums the stems. With `sidechain`, bass, pad and keys duck briefly on every kick (the usual pumping
// effect that keeps the low end clear).
AudioBuffer mixStems(const Song& song, const std::vector<Stem>& stems, double sampleRate, bool sidechain = true);

struct ProjectFiles {
  std::string dir, midi, mix, session;
  std::vector<std::string> stems;
};
// Writes <dir>/stems/*.wav, <dir>/song.mid, <dir>/mix.wav (unmastered) and <dir>/song.wezt.
bool writeProject(const Song& song, const std::string& dir, double sampleRate, ProjectFiles& files, std::string* error = nullptr);

}  // namespace wezt::music
