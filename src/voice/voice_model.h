#pragma once
// A voice model trained from scratch on recordings you give it, on your own computer, in seconds to
// minutes, with no neural network and no downloaded weights.
//
// What it learns: the typical shapes of the speaker's spectral envelope (a codebook of vocal-tract
// colours, found by k-means over cepstral frames), the average envelope, and the pitch range.
// What it does: analyses any other vocal (the pitch, the loudness, the spectral envelope), replaces
// the envelope by the nearest learned shapes, and re-synthesises the voice with a harmonic + noise
// vocoder at the original pitch and loudness.
//
// Be realistic: this is classical statistical voice conversion. It captures the timbre of the voice
// (brightness, nasality, vowel colour) but sounds more synthetic than a neural system such as RVC
// trained on hours of audio on a GPU. It needs at least a minute or two of clear, dry, solo voice;
// more and more varied (different vowels and pitches) is better.
//
// Consent: a model can only be trained when the caller states that the voice is their own or the
// speaker has agreed; the statement and owner name are stored in the model file.
#include <string>
#include <vector>

#include "engine/audio_buffer.h"

namespace wezt::voice {

constexpr int kCep = 24;     // cepstral coefficients per frame (c0 = level)

struct VoiceFrame {
  double time = 0;
  bool voiced = false;
  bool silent = false;
  double f0 = 0;
  double rms = 0;
  float cep[kCep] = {};
};
std::vector<VoiceFrame> analyzeVoice(const AudioBuffer& audio, double sampleRate);

struct VoiceModel {
  std::string name, owner, consent;
  double medianF0 = 0, f0Low = 0, f0High = 0;   // Hz
  double voicedSeconds = 0;
  int codes = 0, unvoicedCodes = 0;
  std::vector<float> meanVoiced, meanUnvoiced;  // kCep - 1 values each (c1..)
  std::vector<float> voicedBook, unvoicedBook;  // codes * (kCep - 1)
};

struct TrainParams {
  std::string name = "voice", owner;
  bool consent = false;       // must be true
  int codes = 96;
  int iterations = 15;
};
struct TrainResult { bool ok = false; std::string message; VoiceModel model; };
TrainResult trainVoiceModel(const std::vector<AudioBuffer>& recordings, double sampleRate, const TrainParams& p);

bool saveVoiceModel(const std::string& path, const VoiceModel& m, std::string* error = nullptr);
bool loadVoiceModel(const std::string& path, VoiceModel& m, std::string* error = nullptr);

struct ConvertParams {
  double pitchShiftSemitones = 0;   // extra shift on top of the octave match
  bool matchRange = true;           // move the pitch by whole octaves toward the model's range
  float codebookWeight = 0.6f;      // 0 = keep the source's own envelope (shifted), 1 = only learned shapes
  bool warpFormants = true;         // fit the vocal-tract length of the source to the model first
  float breath = 0.06f;             // share of noise in voiced sounds
};
// Mono result, same length as the input.
AudioBuffer convertVoice(const AudioBuffer& in, double sampleRate, const VoiceModel& m, const ConvertParams& p = {});

}  // namespace wezt::voice
