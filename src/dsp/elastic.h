#pragma once
// Elastic Audio engine: change the length of audio without changing its pitch (time-stretch), or the
// pitch without the length, all offline.
//
//   Polyphonic  phase vocoder with identity phase locking: full mixes, chords, anything.
//   Rhythmic    the same, but phases are reset on every hit so drums stay sharp.
//   Monophonic  pitch-synchronous (PSOLA): a single voice or instrument, formants kept.
//   Varispeed   plain resampling: length and pitch change together, like tape.
//
// Quality is good for stretches within about 0.5x..2x. Beyond that expect smearing (Polyphonic) or
// roughness (Monophonic). This is clean-room code from the published algorithms.
#include "engine/audio_buffer.h"

namespace wezt::dsp {

enum class ElasticMode { Polyphonic, Rhythmic, Monophonic, Varispeed };
const char* elasticModeName(ElasticMode m);
bool parseElasticMode(const char* text, ElasticMode& m);   // "poly", "rhythmic", "mono", "varispeed"

// `ratio` = new length / old length (2.0 = twice as long, half the tempo). Same pitch.
AudioBuffer elasticStretch(const AudioBuffer& in, double ratio, ElasticMode mode, double sampleRate);
// Same length, pitch moved by `semitones`.
AudioBuffer elasticPitch(const AudioBuffer& in, double semitones, ElasticMode mode, double sampleRate);

}  // namespace wezt::dsp
