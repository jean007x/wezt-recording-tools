#pragma once
// Splits a stereo mix into drums, bass, vocals and other, on this computer.
//
// Two methods:
//  * `separateDsp`: signal processing only (harmonic/percussive median filtering, a bass crossover
//    and a centre-channel mask for vocals). Always available. It is a rough tool: drums and bass come
//    out usefully, vocals only when they sit in the centre, and instruments that share a band bleed
//    into each other. The four stems add back up to the input.
//  * `separateExternal`: runs a neural separator you installed yourself (Demucs, MIT licence) when it
//    is on the PATH. Much better quality. Wezt does not ship or download the model.
#include <string>

#include "engine/audio_buffer.h"

namespace wezt::music {

struct StemSet { AudioBuffer drums, bass, vocals, other; };

struct SeparationOptions {
  double maxSeconds = 300;        // longer input is refused (the spectrograms are held in memory)
  int medianWidth = 17;           // frames / bins in the harmonic-percussive filters
  double bassCutoffHz = 160;
};

// The input must be 1 or 2 channels (mono is treated as dual mono).
bool separateDsp(const AudioBuffer& mix, double sampleRate, StemSet& out, std::string* error = nullptr, const SeparationOptions& opt = {});

// True when a neural separator is installed and can be run.
bool externalSeparatorAvailable(std::string* which = nullptr);
// Runs it on a WAV file and reads the four stems back. Blocking; can take minutes. POSIX only.
bool separateExternal(const std::string& wavPath, const std::string& workDir, StemSet& out, double& sampleRate, std::string* error = nullptr);

}  // namespace wezt::music
