#pragma once
// Reads a WAV file into memory for analysis, with a length cap so a huge file cannot exhaust RAM.
#include <string>

#include "engine/audio_buffer.h"

namespace wezt::ai {

bool loadAudioFile(const std::string& path, AudioBuffer& out, double& sampleRate, std::string* error, double maxSeconds = 900.0);

}  // namespace wezt::ai
