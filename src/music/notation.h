#pragma once
// Turns notes into engraved music: clef, key signature, bars, note values, ties, rests and
// accidentals, ready to draw. It decides WHAT is printed; the window only draws it.
//
// One voice per staff: simultaneous notes with the same start form a chord, and a note that is still
// sounding when the next one starts is shown cut at that point. Times are rounded to sixteenth notes
// for printing only; the notes themselves are never changed.
#include <string>
#include <vector>

#include "music/midi.h"
#include "music/theory.h"

namespace wezt::music {

enum class Clef { Treble, Bass };

struct ScoreNote {
  int midi = 60;
  int step = 28;          // diatonic position: 7 * octave + letter (C4 = 28, middle C)
  int accidental = -2;    // -2 none, -1 flat, 0 natural sign, 1 sharp (what is printed beside the note)
  std::string name;       // "F#4"
};

struct ScoreEvent {
  double beat = 0;        // position in the bar, 0-based
  double length = 0;      // beats
  int value = 2;          // 0 whole, 1 half, 2 quarter, 3 eighth, 4 sixteenth
  int dots = 0;
  bool rest = false;
  bool tieToNext = false;
  std::vector<ScoreNote> notes;   // empty for rests
};

struct ScoreBar { int number = 1; std::vector<ScoreEvent> events; };

struct Score {
  Clef clef = Clef::Treble;
  Key key;
  int fifths = 0;         // key signature: +n sharps, -n flats
  int beatsPerBar = 4;
  std::vector<ScoreBar> bars;
};

Score engrave(const MidiClip& clip, const Key& key, int beatsPerBar = 4, int forceClef = -1);   // forceClef: 0 treble, 1 bass, -1 automatic
int keyFifths(const Key& key);                       // sharps (+) or flats (-) of the key signature
std::string spellPitch(int midi, int fifths);        // "Bb3" in F major, "A#3" in B major
int stepOf(int midi, int fifths);                    // diatonic step of the spelled pitch
int bottomLineStep(Clef c);                          // step of the lowest staff line (E4 treble, G2 bass)

}  // namespace wezt::music
