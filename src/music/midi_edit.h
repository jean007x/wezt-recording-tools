#pragma once
// MIDI editing on note lists: the operations of a piano-roll editor, usable from text, from Claude and
// from code. All functions return a new clip and leave the input alone.
#include <string>

#include "music/midi.h"
#include "music/theory.h"

namespace wezt::music {

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
