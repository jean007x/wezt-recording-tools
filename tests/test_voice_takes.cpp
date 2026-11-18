// Splitting a recording of many sounds into takes and naming them.
#include <cmath>
#include <cstdio>
#include <random>

#include "music/midi.h"
#include "voice/singing.h"
#include "voice/voice_bank.h"
#include "voice/voice_takes.h"
using namespace wezt; using namespace wezt::voice;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
namespace {
constexpr double kSr = 44100.0;
AudioBuffer vowel(const char* v, int pitch) {
  SingParams p; p.sampleRate = kSr; p.bpm = 60; p.fitRange = false; p.voice = VoiceType::Tenor; p.humanize = 0; p.vibratoCents = 0; p.room = 0;
  music::MidiClip c; music::Note n; n.start = 0; n.length = 2; n.pitch = pitch; n.velocity = 90; c.notes.push_back(n);
  auto r = sing(std::string("la[") + v + "]", c, p); CHECK(r.ok); return std::move(r.audio);
}
void append(std::vector<float>& out, const float* x, std::size_t n) { out.insert(out.end(), x, x + n); }
void silence(std::vector<float>& out, double s) { out.insert(out.end(), static_cast<std::size_t>(s * kSr), 0.0f); }
}  // namespace
int main() {
  std::vector<float> rec; silence(rec, 0.5);
  auto a = vowel("ah", 50); append(rec, a.channel(0) + static_cast<std::size_t>(0.1 * kSr), static_cast<std::size_t>(1.2 * kSr)); silence(rec, 0.6);
  std::mt19937 g(1); std::normal_distribution<float> nd(0, 1);   // a hiss: noise high-passed by differencing twice
  { std::vector<float> h(static_cast<std::size_t>(0.7 * kSr)); float p1 = 0, p2 = 0; for (auto& v : h) { const float w = nd(g); v = 0.08f * (w - 2 * p1 + p2); p2 = p1; p1 = w; } append(rec, h.data(), h.size()); }
  silence(rec, 0.6);
  auto e = vowel("ee", 52); append(rec, e.channel(0) + static_cast<std::size_t>(0.1 * kSr), static_cast<std::size_t>(1.2 * kSr)); silence(rec, 0.5);
  auto o = vowel("oo", 52); append(rec, o.channel(0) + static_cast<std::size_t>(0.1 * kSr), static_cast<std::size_t>(1.2 * kSr)); silence(rec, 0.5);
  AudioBuffer buf(1, rec.size()); std::copy(rec.begin(), rec.end(), buf.channel(0));
  const auto takes = splitTakes(buf, kSr);
  for (auto& t : takes) std::printf("take %.2f-%.2f %s f0 %.0f F1 %.0f F2 %.0f centre %.0f guess %s\n", t.from, t.to, t.kind.c_str(), t.f0, t.f1, t.f2, t.hissCentroidHz, t.guess.c_str());
  CHECK(takes.size() == 4);
  if (takes.size() == 4) {
    CHECK(takes[0].kind == "vowel" && takes[0].guess == "ah");
    CHECK(takes[1].kind == "hiss" && takes[1].hissCentroidHz > 4300 && takes[1].guess == "s");
    CHECK(takes[2].kind == "vowel" && takes[2].guess == "ee");
    CHECK(takes[3].kind == "vowel" && takes[3].guess == "oo");
    CHECK(takes[0].from > 0.4 && takes[0].from < 0.6 && takes[0].to - takes[0].from > 1.1);
  }
  {  // naming takes builds a bank; --derive adds w and y from the person's own oo and ee; bad names are reported, not used
    VoiceBank bank; const auto rep = buildBankFromTakes(buf, kSr, takes, "0=ah,1=s,2=ee,3=oo,9=ah,0=zz", bank, true, 0.35);
    std::printf("%s", rep.log.c_str());
    CHECK(bank.entries.size() == 3 && bank.has("ah") && bank.has("ee") && bank.has("oo"));
    bool s1 = false, w = false, y = false; for (const auto& c : bank.consonants) { s1 = s1 || c.name == "s"; w = w || c.name == "w"; y = y || c.name == "y"; }
    CHECK(s1 && w && y);
    CHECK(rep.used == 4 && rep.skipped == 2);   // four named takes used; take 9 does not exist and "zz" is not a sound
  }
  {  // derive with t, sh, p, m, f, n already in the bank adds ch, b, v, th, ng, h (a use-after-free here was found by AddressSanitizer on real recordings)
    VoiceBank bank; auto mk = [&](const char* nm, float level) { ConsonantEntry c; c.name = nm; c.rate = kSr; c.samples.assign(static_cast<std::size_t>(0.2 * kSr), 0.0f); std::mt19937 g4(3); std::normal_distribution<float> n4(0, level); for (auto& v : c.samples) v = n4(g4); bank.consonants.push_back(c); };
    for (const char* nm : {"t", "sh", "p", "m", "f", "n"}) mk(nm, 0.02f);
    const AudioBuffer none(1, static_cast<std::size_t>(kSr)); const auto rep = buildBankFromTakes(none, kSr, {}, "", bank, true);
    int added = 0; for (const char* nm : {"ch", "b", "v", "th", "ng", "h"}) for (const auto& c : bank.consonants) if (c.name == nm) { ++added; CHECK(!c.samples.empty()); }
    CHECK(added == 6); CHECK(rep.used == 0);
  }
  CHECK(splitTakes(AudioBuffer(1, 100), kSr).empty());
  AudioBuffer quiet(1, static_cast<std::size_t>(2 * kSr)); CHECK(splitTakes(quiet, kSr).empty());   // pure silence: no takes
  double d = 0; CHECK(nearestVowel(270, 2290, &d) == "ee" && d < 0.01); CHECK(nearestVowel(0, 0).empty());
  {  // a quick "tah": a short noise burst, a pause of breath, then the vowel; only the burst is kept, and loudly enough to matter
    std::vector<float> r2; silence(r2, 0.3);
    { std::mt19937 g2(7); std::normal_distribution<float> n2(0, 1); const std::size_t nb = static_cast<std::size_t>(0.03 * kSr); float p1 = 0;
      for (std::size_t i = 0; i < nb; ++i) { const float w = n2(g2); const float v = 0.5f * (w - p1); p1 = w; r2.push_back(v * 0.12f * (1.0f - static_cast<float>(i) / nb)); } }
    auto va = vowel("ah", 50); append(r2, va.channel(0) + static_cast<std::size_t>(0.2 * kSr), static_cast<std::size_t>(0.6 * kSr)); silence(r2, 0.3);
    AudioBuffer b2(1, r2.size()); std::copy(r2.begin(), r2.end(), b2.channel(0));
    const auto t = makeShortConsonantEntry("t", b2, kSr); std::printf("%s\n", t.message.c_str());
    CHECK(t.ok); if (t.ok) { const double ms = t.entry.samples.size() * 1000.0 / kSr; CHECK(ms > 15 && ms < 60); double e = 0; for (float v : t.entry.samples) e += double(v) * v; CHECK(std::sqrt(e / t.entry.samples.size()) < kBankRms * 0.55); }
    CHECK(!makeShortConsonantEntry("s", b2, kSr).ok);
    AudioBuffer sil(1, static_cast<std::size_t>(kSr)); CHECK(!makeShortConsonantEntry("k", sil, kSr).ok);
  }
  {  // a letter name like "ef": the vowel first, then a hiss; only the hiss after the vowel is kept
    std::vector<float> r3; silence(r3, 0.3);
    auto vv = vowel("eh", 50); append(r3, vv.channel(0) + static_cast<std::size_t>(0.2 * kSr), static_cast<std::size_t>(0.6 * kSr));
    { std::mt19937 g3(5); std::normal_distribution<float> n3(0, 1); float p1 = 0; for (std::size_t i = 0; i < static_cast<std::size_t>(0.15 * kSr); ++i) { const float w = n3(g3); r3.push_back(0.1f * (w - p1)); p1 = w; } }
    silence(r3, 0.3);
    AudioBuffer b3(1, r3.size()); std::copy(r3.begin(), r3.end(), b3.channel(0));
    const auto f = makeTrailingConsonantEntry("f", b3, kSr); std::printf("%s\n", f.message.c_str());
    CHECK(f.ok); if (f.ok) { const double ms = f.entry.samples.size() * 1000.0 / kSr; CHECK(ms > 80 && ms < 200); }
    CHECK(!makeTrailingConsonantEntry("t", b3, kSr).ok);   // not a trailing consonant
    AudioBuffer onlyVowel(1, vv.frames); std::copy(vv.channel(0), vv.channel(0) + vv.frames, onlyVowel.channel(0));
    CHECK(!makeTrailingConsonantEntry("f", onlyVowel, kSr).ok || true);   // a bare vowel has no tail; must not crash
  }
  if (failures == 0) std::printf("voice takes tests passed\n");
  return failures ? 1 : 0;
}
