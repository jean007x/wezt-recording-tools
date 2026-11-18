#pragma once
// Fit your own recorded vocal to a melody: each sung note is retuned to the target note and
// stretched to its length, so your real voice sings a song you did not sing in time. It also builds
// a harmony line from copies of your own recording.
//
// This is signal processing on YOUR recording. It does not create a voice model, so it cannot make a
// voice you did not record, and the vocal keeps its own timbre. Large shifts sound less natural
// (PSOLA keeps the formants in place, but quality drops past about 7 semitones); choose a melody close to your range.
#include <string>

#include "engine/audio_buffer.h"
#include "music/midi.h"
#include "music/theory.h"

namespace wezt::music {

struct VocalFitParams {
  double bpm = 120;
  Key key;                  // for the diatonic harmony
  bool harmony = true;
  double maxStretch = 2.0;  // longest/shortest time change applied to one note
  double harmonyDb = -9.0;
};

struct VocalFit {
  bool ok = false;
  std::string message;
  AudioBuffer lead, harmony;     // mono, same length (the melody's length); harmony is empty if off
  int sourceNotes = 0, targetNotes = 0;
};

VocalFit fitVocal(const AudioBuffer& vocal, double sampleRate, const MidiClip& melody, const VocalFitParams& p = {});

struct ChoirParams {
  Key key;
  int voices = 4;          // 1..8 extra voices
  double detuneCents = 12; // each voice is slightly out of tune with the others, like real singers
  double spreadMs = 28;    // each voice enters a little later than the last
  float voiceDb = -9.f;
};
// One take of your voice becomes a stacked choir: copies of the same recording moved to chord-like
// scale steps (3rd, 5th, below, octave...), detuned and spread in time and across the stereo field.
// Stereo, as long as the vocal. Uses only that recording.
AudioBuffer makeChoir(const AudioBuffer& vocal, double sampleRate, const ChoirParams& p = {});

}  // namespace wezt::music
