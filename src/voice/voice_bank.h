#pragma once
// A voice bank: short recordings of one person singing or holding vowels, used as the raw material for
// singing. Unlike the voice model (which only captures a timbre and re-synthesizes it), the bank keeps
// the actual audio of the person: the singer re-pitches it with PSOLA (pitch-synchronous overlap-add), which
// moves the pitch while the vocal-tract colour, breath and texture of the recording stay.
//
// Record each vowel as one steady, dry, clearly pitched note (about 1.5 s or more), at a comfortable pitch.
// Notes more than about 5 semitones from the recording sound increasingly like a different, smaller or
// larger person (the formants do not follow); record two pitches of a vowel to cover a wide range.
// Consent: only for your own voice or a speaker who agreed; the statement is stored in the file.
#include <memory>
#include <string>
#include <vector>

#include "engine/audio_buffer.h"

namespace wezt::voice {

struct BankEntry {
  std::string vowel;            // ah ee eh ae ih oh oo uh er
  double f0 = 0;                // Hz, median of the steady part
  double rate = 48000;
  std::vector<float> samples;   // the steady part, normalized to a fixed RMS
  std::vector<int> marks;       // pitch marks (sample indices of one peak per period)
};

// A consonant cut from a recording of a syllable like "sah" or "tah": the sound before the vowel starts.
struct ConsonantEntry {
  std::string name;             // p t k b d g f v s z sh ch th h m n l r w y j ng
  double rate = 48000;
  std::vector<float> samples;   // scaled to the loudness it had next to its own vowel (vowels are stored at kBankRms)
};

struct VoiceBank {
  std::string name, owner, consent;
  std::vector<BankEntry> entries;
  std::vector<ConsonantEntry> consonants;
  // The recorded consonant for a letter or digraph, or null (then the synthesizer sings it). c->k, q->k, x->s.
  const ConsonantEntry* findConsonant(const std::string& c) const;
  // The entry to use for a vowel and a pitch: the closest recorded pitch; a missing vowel falls back to a
  // similar one (ae->eh, ih->ee, uh->ah, er->eh). Null when nothing is close.
  const BankEntry* find(const std::string& vowel, double f0) const;
  bool has(const std::string& vowel) const { return find(vowel, 0) != nullptr; }
};

// Empty when the measured formants fit the label; otherwise a warning naming the closer vowel.
std::string vowelLabelWarning(const std::string& label, double f1, double f2);

struct BankResult { bool ok = false; std::string message; BankEntry entry; };
// Cuts the steadiest voiced stretch out of a recording and finds its pitch marks.
// minSteadySeconds: how long the vowel must stay steady. 0.35 s for a held note; a vowel inside a spoken letter name ("bee") is shorter, about 0.15 s.
BankResult makeBankEntry(const std::string& vowel, const AudioBuffer& audio, double sampleRate, double minSteadySeconds = 0.35);

struct ConsonantResult { bool ok = false; std::string message; ConsonantEntry entry; };
// `audio` holds the person saying or singing the consonant followed by a steady "ah" (for example "tah"): the
// part before the vowel is cut out and kept at its real loudness relative to the vowel.
// For p, t, k (and the voiced b, d, g, j, z) said quickly with hardly any vowel after them (no held "ah" needed).
ConsonantResult makeShortConsonantEntry(const std::string& name, const AudioBuffer& audio, double sampleRate);
// For f, s, sh, th, n, m, l, ng said after a vowel ("ef", "en", "em", "el", "ess").
ConsonantResult makeTrailingConsonantEntry(const std::string& name, const AudioBuffer& audio, double sampleRate);
ConsonantResult makeConsonantEntry(const std::string& name, const AudioBuffer& audio, double sampleRate);
bool isConsonantName(const std::string& name);

bool saveVoiceBank(const std::string& path, const VoiceBank& b, std::string* error = nullptr);
bool loadVoiceBank(const std::string& path, VoiceBank& b, std::string* error = nullptr);

// Streaming PSOLA player for one bank entry: next(f0) returns one output sample at the requested pitch.
class BankVoice {
 public:
  BankVoice(const BankEntry& e, double outRate);
  double next(double f0);
 private:
  void grain(double f0);
  std::vector<float> src_;       // resampled to the output rate
  std::vector<int> marks_;
  std::vector<float> ring_;
  std::size_t rd_ = 0;
  double untilGrain_ = 0, srcPos_ = 0;   // output samples until the next grain; read position as a mark index
  int dir_ = 1;
  double rate_;
  double period_ = 0;            // recorded period in output samples
};

constexpr double kBankRms = 0.1;

}  // namespace wezt::voice
