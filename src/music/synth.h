#pragma once
// Built-in sound sources that turn notes into audio, offline. They exist so that a generated song
// can be heard with no plug-ins installed; they are plain subtractive synthesis and synthesised
// drums, not recordings. Real instruments (VST3, CLAP, the sampler) replace them in a normal session.
#include <string>
#include <vector>

#include "engine/audio_buffer.h"
#include "music/midi.h"

namespace wezt::music {

struct SynthPreset {
  std::string name = "keys";
  float sine = 0, tri = 0, saw = 1, square = 0;  // oscillator mix
  int unison = 1;                                  // voices per note, spread across the stereo field
  float detuneCents = 8;
  float subLevel = 0;                              // sine one octave down
  float cutoffHz = 3000, envToCutoff = 0;          // static cutoff plus an envelope that opens it (in Hz)
  float attack = 0.005f, decay = 0.2f, sustain = 0.7f, release = 0.2f;  // seconds, level
  float gainDb = -6;
  float pan = 0;                                   // -1 left .. 1 right
};

// bass, keys, pad, lead, pluck. Unknown names give "keys".
SynthPreset synthPreset(const std::string& name);
std::vector<std::string> synthPresetNames();

// Renders notes (beats) at `bpm`. Returns stereo audio long enough for the last release.
AudioBuffer renderSynth(const MidiClip& clip, const SynthPreset& preset, double bpm, double sampleRate);
// General MIDI drum notes (36 kick, 38 snare, 39 clap, 42/46 hats, 45 tom, 49 crash, 51 ride).
AudioBuffer renderDrums(const MidiClip& clip, double bpm, double sampleRate);

}  // namespace wezt::music
