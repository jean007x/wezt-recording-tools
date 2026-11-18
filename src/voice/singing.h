#pragma once
// Singing from lyrics, built from scratch with no trained model: the lyrics are split into syllables
// with simple English spelling rules, each syllable gets a vowel (a set of formants) and consonants
// (noise bursts, nasal and voiced sounds), and a glottal pulse source at the melody's pitch is shaped
// by those formants. It sounds like a classic formant synthesizer: clear and intelligible when the
// lyrics are simple; humanized with per-cycle jitter/shimmer, scooped onsets, late vibrato, a singer's formant and a small room, but still synthetic. Spelling rules are approximate; write [ah] [ee] [oh] [oo] [eh] [ae]
// [uh] [er] after a syllable to force its vowel (for example "lo[oh]ve").
#include <string>
#include <vector>

#include "engine/audio_buffer.h"
#include "music/midi.h"
#include "voice/voice_bank.h"

namespace wezt::voice {

struct Syllable {
  std::string text;      // as written, lower case
  std::string onset, coda;   // consonant letters before and after the vowel
  std::string vowel;     // ah ee eh ae oh oo uh er ih
  bool wordStart = false;
};

// Splits lyrics into syllables. Words are separated by spaces, line breaks and punctuation; a hyphen
// forces a syllable break ("won-der-ful").
std::vector<Syllable> syllabify(const std::string& lyrics);

enum class VoiceType { Bass, Tenor, Alto, Soprano };

struct SingParams {
  VoiceType voice = VoiceType::Alto;
  double bpm = 120;
  bool fitRange = true;        // move the melody by whole octaves into the voice's comfortable range
  int choirVoices = 1;         // 1 = solo; more voices are detuned and spread in time
  double vibratoHz = 5.9, vibratoCents = 24;   // rate and half-depth: about 50-65 cents peak to peak, typical of pop and jazz singing; classical goes to 100-180
  double breath = 0.04;        // 0..0.3, pulsed airy noise; softer notes are breathier
  double humanize = 0.7;       // 0..1: how much the pitch, timing, loudness and vibrato wander like a real singer (0 = machine-steady)
  double room = 0.15;          // 0..1: a small, short room around the voice; a bone-dry voice sounds synthetic
  double singerFormantDb = 5;  // 0..12: the 2.5-3.5 kHz "ring" trained singers have, that lets a voice carry
  // voice character (see applyCharacter): different people, not just different ranges
  double formantShift = 1.0;   // vocal-tract length: <1 = longer, bigger, darker; >1 = shorter, smaller, child-like
  double brightness = 1.0;     // scales the high-frequency lift (0.4 = dull and warm, 1.6 = bright and forward)
  double roughness = 0.0;      // 0..1: ~65 Hz irregular amplitude modulation and extra jitter (grit, rasp)
  double fry = 0.0;            // 0..1: creaky, pulsed voice when a note ends
  double effortBias = 0.0;     // -0.3..0.3: sung softer or harder than the notes' loudness suggests
  bool normalize = true;             // false leaves the absolute level (tests that compare levels)
  const VoiceBank* bank = nullptr;   // recordings of a real person: vowels come from the audio instead of the synthesizer
  double sampleRate = 48000;
  std::uint64_t seed = 1;
};

// Names: warm_alto, bright_pop, breathy_pop, gritty_rock, operatic_soprano, deep_bass, tenor_crooner,
// boyish, child, airy_whisper, choirboy, folk_storyteller. Sets the voice type and the character fields.
bool applyCharacter(const std::string& name, SingParams& p);
std::vector<std::string> characterNames();

struct SingResult {
  bool ok = false;
  std::string message;
  AudioBuffer audio;           // mono
  int syllables = 0, notes = 0, usedSyllables = 0;
  int octaveShift = 0;
  int bankConsonants = 0, bankConsonantsSynth = 0;   // consonants played from the person's recordings / left to the synthesizer
  int bankNotes = 0, bankFarNotes = 0, bankMissing = 0;   // notes sung from the bank; those more than 5 semitones from the recording; notes whose vowel was not recorded
};

SingResult sing(const std::string& lyrics, const music::MidiClip& melody, const SingParams& p);
const char* voiceName(VoiceType v);
bool parseVoice(const std::string& s, VoiceType& out);

}  // namespace wezt::voice
