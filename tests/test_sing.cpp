#include <cmath>
#include <cstdio>

#include "music/audio_to_midi.h"
#include "voice/singing.h"

using namespace wezt;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static double bandEnergy(const AudioBuffer& b, double sr, std::size_t from, std::size_t to, double f0, double f1) {
  double e = 0;
  for (double f = f0; f <= f1; f += 40) {   // Goertzel-style sweep
    double re = 0, im = 0;
    for (std::size_t i = from; i < to; ++i) { const double w = 2 * M_PI * f * static_cast<double>(i) / sr; re += b.channel(0)[i] * std::cos(w); im += b.channel(0)[i] * std::sin(w); }
    e += re * re + im * im;
  }
  return e;
}

int main() {
  using namespace voice;
  auto txt = [](const std::vector<Syllable>& v) { std::string s; for (auto& x : v) s += x.text + "|"; return s; };
  CHECK(txt(syllabify("hello world")) == "hel|lo|world|");
  CHECK(txt(syllabify("won-der-ful night")) == "won|der|ful|night|");
  CHECK(txt(syllabify("wonder")) == "won|der|");
  CHECK(txt(syllabify("Love, love!\nme")) == "lov|lov|me|");     // punctuation and line breaks separate words; silent e
  CHECK(syllabify("").empty() && syllabify("  ,, !! ").empty());
  auto s = syllabify("night");
  CHECK(s.size() == 1 && s[0].vowel == "ah" && s[0].onset == "n" && s[0].coda == "t");
  s = syllabify("lo[uh]ve");
  CHECK(s.size() == 1 && s[0].vowel == "uh");
  s = syllabify("tree");
  CHECK(s.size() == 1 && s[0].vowel == "ee" && s[0].onset == "tr");
  s = syllabify("moon");
  CHECK(s[0].vowel == "oo");
  CHECK(syllabify("sing")[0].vowel == "ih");

  const double sr = 22050;
  music::MidiClip mel;
  for (int i = 0; i < 4; ++i) mel.notes.push_back({57, i * 1.0, 0.9, 100});   // A3 = 220 Hz, 120 bpm -> half a second each
  SingParams p; p.sampleRate = sr; p.bpm = 120; p.fitRange = false; p.voice = VoiceType::Tenor;
  auto r = sing("la la la la", mel, p);
  CHECK(r.ok && r.syllables == 4 && r.notes == 4 && r.usedSyllables == 4);
  CHECK(r.audio.frames > static_cast<std::size_t>(2.0 * sr));
  float pk = 0; bool finite = true;
  for (std::size_t i = 0; i < r.audio.frames; ++i) { const float v = r.audio.channel(0)[i]; pk = std::max(pk, std::fabs(v)); if (!std::isfinite(v)) finite = false; }
  CHECK(finite && pk > 0.3f && pk <= 0.61f);
  // the pitch is the melody's: median of the voiced frames in note 2 within half a semitone of A3
  const auto fr = music::trackPitch(r.audio, sr);
  std::vector<double> m;
  for (auto& f : fr) if (f.hz > 0 && f.confidence > 0.5 && f.time > 0.6 && f.time < 0.95) m.push_back(69 + 12 * std::log2(f.hz / 440.0));
  CHECK(m.size() > 3);
  if (!m.empty()) { std::sort(m.begin(), m.end()); CHECK(std::fabs(m[m.size() / 2] - 57) < 0.6); }
  // silence after the last note (the tail is only a release)
  CHECK(std::fabs(r.audio.channel(0)[r.audio.frames - 5]) < 0.01f);

  // vowels differ: "ee" has far more energy around 2.3 kHz than "ah"
  music::MidiClip one; one.notes = {{57, 0, 2, 100}};
  const auto ee = sing("ee", one, p), ah = sing("ah", one, p);
  CHECK(ee.ok && ah.ok);
  const std::size_t a = static_cast<std::size_t>(0.3 * sr), b = static_cast<std::size_t>(0.8 * sr);
  const double rEe = bandEnergy(ee.audio, sr, a, b, 2000, 2600) / bandEnergy(ee.audio, sr, a, b, 600, 1000);
  const double rAh = bandEnergy(ah.audio, sr, a, b, 2000, 2600) / bandEnergy(ah.audio, sr, a, b, 600, 1000);
  CHECK(rEe > 3.0 * rAh);

  // more notes than syllables: melisma; fewer notes: syllables left out and said so
  auto m1 = sing("hey", mel, p);
  CHECK(m1.ok && m1.usedSyllables == 1 && m1.message.find("melisma") != std::string::npos);
  music::MidiClip two; two.notes = {{57, 0, 1, 100}, {59, 1, 1, 100}};
  auto m2 = sing("one two three four", two, p);
  CHECK(m2.ok && m2.usedSyllables == 2 && m2.message.find("left out") != std::string::npos);

  // range fitting moves a very high melody into the voice's range
  music::MidiClip hi; hi.notes = {{93, 0, 1, 100}, {95, 1, 1, 100}};
  SingParams ph = p; ph.fitRange = true; ph.voice = VoiceType::Bass;
  auto rh = sing("la la", hi, ph);
  CHECK(rh.ok && rh.octaveShift < 0);

  // choir: still bounded and finite
  SingParams pc = p; pc.choirVoices = 6;
  auto c = sing("ah", one, pc);
  CHECK(c.ok);
  float cp = 0; for (std::size_t i = 0; i < c.audio.frames; ++i) { CHECK(std::isfinite(c.audio.channel(0)[i])); cp = std::max(cp, std::fabs(c.audio.channel(0)[i])); }
  CHECK(cp <= 0.61f);

  // bad input
  CHECK(!sing("", mel, p).ok && !sing("la", music::MidiClip{}, p).ok);
  SingParams bad = p; bad.bpm = 0; CHECK(!sing("la", mel, bad).ok);
  // consonant-heavy lyrics, every letter, odd characters: must not crash
  music::MidiClip many; for (int i = 0; i < 12; ++i) many.notes.push_back({60 + i % 5, i * 0.5, 0.45, 90});
  auto crazy = sing("strengths sixths! x-ray qu1ck 123 zzz hmm [bogus] [ah", many, p);
  CHECK(crazy.ok || !crazy.message.empty());
  VoiceType vt; CHECK(parseVoice("Soprano", vt) && vt == VoiceType::Soprano && !parseVoice("robot", vt));

  std::printf(failures ? "FAILED\n" : "sing tests passed\n");
  return failures;
}
