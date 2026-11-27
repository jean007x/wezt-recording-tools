#pragma once
// MIDI editing on note lists: the operations of a piano-roll editor, usable from text, from Claude and
// from code. All functions return a new clip and leave the input alone.
#include <string>

#include "music/midi.h"
#include "music/theory.h"

namespace wezt::music {

// Real-Time Properties: changes applied when a part is played (the part's notes stay as they are). Transpose in semitones, velocity added to every
// note, note lengths as a percentage, and a quantize grid in beats (0 = off) with its strength 0..1.
struct RealTime {
  int transpose = 0, velocity = 0;
  double lengthPct = 100.0, quantizeGrid = 0.0, strength = 1.0;
  bool active() const { return transpose != 0 || velocity != 0 || lengthPct != 100.0 || quantizeGrid > 0.0; }
};
MidiClip applyRealTime(const MidiClip& c, const RealTime& rt);

MidiClip quantizeMidi(const MidiClip& c, double gridBeats, double strength = 1.0, double swing = 0.0, bool quantizeLengths = false);
MidiClip transposeMidi(const MidiClip& c, int semitones, bool toKey = false, const Key& key = {});   // toKey: snap results to the scale
MidiClip humanizeMidi(const MidiClip& c, double timingBeats, int velocityRange, std::uint64_t seed);
MidiClip legatoMidi(const MidiClip& c, double gapBeats = 0.0);                 // each note ends where the next begins
MidiClip scaleVelocity(const MidiClip& c, double factor, int offset = 0);      // v' = v * factor + offset, 1..127
MidiClip cropMidi(const MidiClip& c, double fromBeat, double toBeat);          // keeps notes inside, shifted to start at 0
MidiClip removeShortNotes(const MidiClip& c, double minBeats);
MidiClip snapToScale(const MidiClip& c, const Key& key);

// Notes written out bar by bar, for reading ("score view in text"): bar 3: C4 (beat 1, 1 beat) ...
std::string describeMidi(const MidiClip& c, int beatsPerBar = 4);
std::string pitchName(int midiNote);   // 60 -> "C4"

}  // namespace wezt::music
