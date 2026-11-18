#pragma once
// Hum a melody or beatbox a rhythm and get notes back. Pure signal processing (YIN pitch tracking,
// onset detection, simple spectral classification): it works best on one clear voice in a quiet room.
#include <vector>

#include "engine/audio_buffer.h"
#include "music/midi.h"
#include "music/theory.h"

namespace wezt::music {

struct PitchFrame { double time = 0, hz = 0, confidence = 0; };  // hz == 0: unvoiced
std::vector<PitchFrame> trackPitch(const AudioBuffer& audio, double sampleRate, double minHz = 70, double maxHz = 1000);

struct AudioToMidiParams {
  double bpm = 120;
  double gridBeats = 0;       // quantise starts to this grid (0.25 = sixteenths); 0 = keep the performance
  bool snapToKey = false;     // move out-of-key notes to the nearest scale note
  Key key;
  double minNoteMs = 80;
  float gateDb = -42.f;       // quieter than this (relative to the loudest part) is silence
};
// One voice, one note at a time. Pitch bends and vibrato are folded into the nearest semitone.
MidiClip audioToMidi(const AudioBuffer& audio, double sampleRate, const AudioToMidiParams& p = {});
// Beatboxing or tapping: kick (low), snare (mid, noisy), hat (high, noisy) as General MIDI drum notes.
MidiClip beatboxToDrums(const AudioBuffer& audio, double sampleRate, const AudioToMidiParams& p = {});

}  // namespace wezt::music
