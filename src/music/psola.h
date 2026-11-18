#pragma once
// Time-domain pitch-synchronous overlap-add (TD-PSOLA). Changes the pitch and the length of a
// monophonic voiced sound (one voice, one note at a time) while keeping its vocal-tract colour
// (formants) in place, so a shifted vocal does not turn into a chipmunk or a monster.
#include <vector>

namespace wezt::music {

// `pitchRatio` 2.0 = one octave up. `outLen` = number of frames wanted (stretches or squeezes in
// time). Unpitched stretches are copied at their own pitch. Works best for shifts within about +-7
// semitones and stretches between 0.5x and 2x.
std::vector<float> psola(const std::vector<float>& x, double sampleRate, double pitchRatio, std::size_t outLen);

}  // namespace wezt::music
