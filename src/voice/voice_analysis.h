#pragma once
// Measurements of a sung or spoken voice, using the standard methods of voice research: cycle-by-cycle
// periods from the waveform (jitter, shimmer, vibrato), harmonics-to-noise ratio from the
// autocorrelation, the alpha ratio from the average spectrum, and formants from linear prediction.
// They exist so the singing synthesizer can be compared with numbers from real singers, and so you
// can run the same measurements on a recording of your own voice (tools/voice_analyze.cpp).
//
// The window analysed should hold ONE sustained pitch (a held vowel; vibrato and a drift of up to a
// semitone are fine). Use findSteadySegment() to pick one out of a longer recording.
#include <string>
#include <vector>

#include "engine/audio_buffer.h"

namespace wezt::voice {

struct VoiceMeasures {
  bool ok = false;
  std::string message;           // why it failed, when it did
  double from = 0, to = 0;       // seconds analysed
  double f0Hz = 0;               // median fundamental
  int periods = 0;               // glottal cycles measured
  double vibratoRateHz = 0;      // 0 = no vibrato found
  double vibratoExtentCents = 0; // peak to peak
  double vibratoStrength = 0;    // 0..1: how regular the modulation is (autocorrelation peak)
  double jitterLocalPct = 0, jitterRapPct = 0;     // cycle-to-cycle period variation, vibrato and drift excluded for RAP
  double shimmerLocalPct = 0, shimmerApq3Pct = 0;  // cycle-to-cycle amplitude variation
  double hnrDb = 0;              // harmonics-to-noise ratio
  double alphaRatioDb = 0;       // energy 1-5 kHz relative to 50 Hz-1 kHz (higher = brighter, "ringing" voices are higher)
  double formantHz[3] = {0, 0, 0};   // F1..F3 by linear prediction (0 = not found)
};

// Mono audio (channel 0 is used). toSec < 0 means "to the end".
VoiceMeasures analyzeVoice(const AudioBuffer& audio, double sampleRate, double fromSec = 0, double toSec = -1);

// Longest stretch of at least `minSeconds` with a steady voiced pitch (within about a semitone).
bool findSteadySegment(const AudioBuffer& audio, double sampleRate, double minSeconds, double& fromSec, double& toSec);

// Vibrato depth (cents peak to peak) in consecutive windows, to see when vibrato starts on a held note.
std::vector<double> vibratoEnvelope(const AudioBuffer& audio, double sampleRate, double fromSec, double toSec, double windowSec);

// One paragraph of text with the numbers next to commonly cited reference values.
std::string describeMeasures(const VoiceMeasures& m);

}  // namespace wezt::voice
