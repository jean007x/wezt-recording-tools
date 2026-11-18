#pragma once
// Splits one long recording of many separate sounds (vowels, syllables like "sah", hissing sounds) into
// takes at the silences, and measures each take so it can be named and put into a voice bank.
// Guesses come from measurements only (formants, how bright the hiss is): the person's own list of what
// they said, in order, is better than any guess, so the tool that uses this prints the table and lets the
// caller choose the names.
#include <string>
#include <vector>

#include "engine/audio_buffer.h"
#include "voice/voice_bank.h"

namespace wezt::voice {

struct Take {
  double from = 0, to = 0;          // seconds
  double voicedShare = 0;           // 0..1 of the take with a clear pitch
  double voicedFrom = -1;           // seconds into the take where the pitch starts (-1: never)
  double f0 = 0;                    // median pitch of the voiced part
  double f1 = 0, f2 = 0;            // formants of the voiced part (0 when too short to measure)
  double hissCentroidHz = 0;        // spectral centre of the unvoiced part (a high value is an "s", a lower one an "sh")
  std::string kind;                 // "vowel", "syllable" (consonant then vowel), "hiss", "voiced-short" or "unclear"
  std::string guess;                // nearest vowel, or "s" / "sh" for a hiss; empty for syllables
};

// minPause: silence (seconds) that separates takes. minTake: shorter sounds are dropped.
std::vector<Take> splitTakes(const AudioBuffer& audio, double sampleRate, double minPause = 0.12, double minTake = 0.15);

// The closest vowel name for measured F1 and F2 (adult male reference values), and the distance in octaves.
std::string nearestVowel(double f1, double f2, double* distanceOctaves = nullptr);

struct BuildReport { std::string log; int used = 0, skipped = 0; };

// Puts named takes into a voice bank. `use` is "idx=name,idx=name" (see tools/voice_takes.cpp for the names):
// vowels (ah ee eh ae ih oh oo uh er), consonants said before a vowel ("tah"), quick stops (p t k b d g j z),
// hisses (s sh f h th), and "name@tail" for a consonant said after its vowel ("1=f@tail" for "ef").
// derive: fill the consonants that were not recorded from the person's own sounds in the bank.
// minSteady: 0.35 for held vowels, 0.15 for vowels inside spoken letter names.
BuildReport buildBankFromTakes(const AudioBuffer& mono, double sampleRate, const std::vector<Take>& takes, const std::string& use, VoiceBank& bank, bool derive = false, double minSteady = 0.35);

}  // namespace wezt::voice
