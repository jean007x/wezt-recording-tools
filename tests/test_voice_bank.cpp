// The recorded-vowel voice bank: pitch marks, PSOLA re-pitching, file format, fallbacks, levels.
// The "recordings" are held vowels from the formant singer (no real person is available to the test),
// which is enough to check the mechanics: the pitch lands where it should, the level matches, the
// recording's own spectrum is kept, and damaged files are refused.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include "music/audio_to_midi.h"
#include "voice/singing.h"
#include "voice/voice_bank.h"

using namespace wezt;
using namespace wezt::voice;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
namespace {
constexpr double kSr = 44100.0;
music::MidiClip note(int pitch, double start, double len, int vel = 90) { music::MidiClip c; music::Note n; n.start = start; n.length = len; n.pitch = pitch; n.velocity = vel; c.notes.push_back(n); return c; }
SingParams base() { SingParams p; p.bpm = 120; p.sampleRate = kSr; p.fitRange = false; p.voice = VoiceType::Tenor; p.room = 0; return p; }
AudioBuffer held(const char* vowel, int pitch) {   // a steady 2.5 s recording
  SingParams p = base(); p.humanize = 0; p.vibratoCents = 0; p.breath = 0.02;
  auto r = sing(std::string("la[") + vowel + "]", note(pitch, 0, 5), p); CHECK(r.ok); if (!r.ok) std::printf("%s\n", r.message.c_str()); return std::move(r.audio);
}
double rms(const AudioBuffer& a, double t0, double t1) { double s = 0; std::size_t n = 0; for (std::size_t i = static_cast<std::size_t>(t0 * kSr); i < static_cast<std::size_t>(t1 * kSr) && i < a.frames; ++i, ++n) s += double(a.channel(0)[i]) * a.channel(0)[i]; return std::sqrt(s / std::max<std::size_t>(1, n)); }
double pitchIn(const AudioBuffer& a, double t0, double t1) {
  const auto fr = music::trackPitch(a, kSr); std::vector<double> hz;
  for (auto& f : fr) if (f.time >= t0 && f.time < t1 && f.hz > 0 && f.confidence > 0.4) hz.push_back(f.hz);
  if (hz.empty()) return 0; std::sort(hz.begin(), hz.end()); return hz[hz.size() / 2];
}
double bright(const AudioBuffer& a, double t0, double t1) {   // energy of the difference signal relative to the signal
  double e = 0, d = 0; for (std::size_t i = static_cast<std::size_t>(t0 * kSr) + 1; i < static_cast<std::size_t>(t1 * kSr) && i < a.frames; ++i) { const double x = a.channel(0)[i], y = a.channel(0)[i - 1]; e += x * x; d += (x - y) * (x - y); }
  return e > 0 ? d / e : 0;
}
}  // namespace

int main() {
  VoiceBank bank; bank.name = "test"; bank.owner = "me"; bank.consent = "own voice";
  for (const char* v : {"ah", "ee", "oo"}) {
    const auto r = makeBankEntry(v, held(v, 57), kSr);
    CHECK(r.ok); if (!r.ok) { std::printf("%s\n", r.message.c_str()); continue; }
    CHECK(std::fabs(r.entry.f0 / 220.0 - 1.0) < 0.02);
    CHECK(r.entry.marks.size() > 50);
    // the marks are about one period apart
    double worst = 0; for (std::size_t i = 1; i < r.entry.marks.size(); ++i) worst = std::max(worst, std::fabs((r.entry.marks[i] - r.entry.marks[i - 1]) * r.entry.f0 / kSr - 1.0));
    CHECK(worst < 0.25);
    bank.entries.push_back(r.entry);
  }
  CHECK(bank.entries.size() == 3);

  {  // refuses what cannot be used
    AudioBuffer shortA(1, 1000); CHECK(!makeBankEntry("ah", shortA, kSr).ok);
    AudioBuffer silent(1, static_cast<std::size_t>(kSr * 2)); CHECK(!makeBankEntry("ah", silent, kSr).ok);
    AudioBuffer noise(1, static_cast<std::size_t>(kSr * 2)); unsigned x = 1; for (std::size_t i = 0; i < noise.frames; ++i) { x = x * 1664525u + 1013904223u; noise.channel(0)[i] = ((x >> 8) & 0xffff) / 32768.f - 1.f; }
    CHECK(!makeBankEntry("ah", noise, kSr).ok);
  }

  {  // file round trip, and damaged files are rejected
    const std::string f = "voicebank_test.wvb";
    std::string err; CHECK(saveVoiceBank(f, bank, &err));
    VoiceBank b2; CHECK(loadVoiceBank(f, b2, &err));
    CHECK(b2.entries.size() == 3 && b2.entries[0].samples == bank.entries[0].samples && b2.entries[1].marks == bank.entries[1].marks && b2.owner == "me" && b2.consent == "own voice");
    std::ifstream in(f, std::ios::binary); std::string all((std::istreambuf_iterator<char>(in)), {});
    for (std::size_t cut : {std::size_t(3), std::size_t(20), all.size() / 2, all.size() - 1}) { std::ofstream o("voicebank_bad.wvb", std::ios::binary); o.write(all.data(), static_cast<std::streamsize>(cut)); o.close(); VoiceBank b3; CHECK(!loadVoiceBank("voicebank_bad.wvb", b3, &err)); }
    std::string bad = all; bad[0] = 'X'; { std::ofstream o("voicebank_bad.wvb", std::ios::binary); o.write(bad.data(), static_cast<std::streamsize>(bad.size())); } VoiceBank b4; CHECK(!loadVoiceBank("voicebank_bad.wvb", b4, &err));
    std::remove(f.c_str()); std::remove("voicebank_bad.wvb");
  }

  {  // singing from the bank: pitch, level, spectrum, determinism
    SingParams p = base(); p.humanize = 0; p.vibratoCents = 0;
    SingParams q = p; q.bank = &bank;
    for (int midi : {57, 60, 62}) {
      auto r = sing("la[ah]", note(midi, 0, 3), q); CHECK(r.ok);
      const double want = 440.0 * std::pow(2.0, (midi - 69) / 12.0), got = pitchIn(r.audio, 0.6, 1.4);
      const double cents = got > 0 ? 1200.0 * std::log2(got / want) : 9999;
      std::printf("bank note %d: wanted %.1f Hz, got %.1f Hz (%.1f cents)\n", midi, want, got, cents);
      CHECK(std::fabs(cents) < 25);
      CHECK(r.bankNotes == 1);
    }
    auto bk = sing("la[ah]", note(60, 0, 3), q);
    {  // absolute level (no final peak normalization): a recorded vowel sits at the loudness of a synthesized one, so consonants of either kind balance with it
      SingParams pn = p, qn = q; pn.normalize = qn.normalize = false;
      for (const char* v : {"ee", "oo"}) {
        const auto b2 = sing(std::string("la[") + v + "]", note(60, 0, 3), qn), s2 = sing(std::string("la[") + v + "]", note(60, 0, 3), pn);
        const double d = 20 * std::log10(rms(b2.audio, 0.6, 1.4) / rms(s2.audio, 0.6, 1.4));
        std::printf("level %s: recorded vs synthesized %.1f dB\n", v, d);
        CHECK(std::fabs(d) < 3.5);
      }
    }
    // no DC, no NaN
    double mean = 0, pk = 0; for (std::size_t i = 0; i < bk.audio.frames; ++i) { const double v = bk.audio.channel(0)[i]; CHECK(std::isfinite(v)); mean += v; pk = std::max(pk, std::fabs(v)); }
    CHECK(std::fabs(mean / static_cast<double>(bk.audio.frames)) < 0.01 * pk);
    // the recording's own spectrum is kept: "oo" (dark) stays darker than "ee" (bright) when sung from the bank, like the recordings
    auto oo = sing("la[oo]", note(60, 0, 3), q), ee = sing("la[ee]", note(60, 0, 3), q);
    CHECK(bright(ee.audio, 0.6, 1.4) > 1.3 * bright(oo.audio, 0.6, 1.4));
    auto bk2 = sing("la[ah]", note(60, 0, 3), q); CHECK(bk2.audio.frames == bk.audio.frames);
    bool same = true; for (std::size_t i = 0; i < bk.audio.frames; ++i) if (bk.audio.channel(0)[i] != bk2.audio.channel(0)[i]) { same = false; break; }
    CHECK(same);
  }

  {  // fallbacks and reports
    SingParams q = base(); q.bank = &bank;
    auto r = sing("la[uh]", note(57, 0, 2), q); CHECK(r.ok && r.bankNotes == 1 && r.bankMissing == 0);   // uh -> ah
    auto m = sing("la[eh]", note(57, 0, 2), q); CHECK(m.ok && m.bankMissing == 1);                         // no eh/ae recorded: the synthesizer fills in
    CHECK(m.audio.frames > 0);
    auto far = sing("la[ah]", note(72, 0, 2), q); CHECK(far.ok && far.bankFarNotes == 1);                 // an octave and a half away from the recording
    // a full phrase with legato vowel changes stays finite and loud enough
    music::MidiClip c; const int pitches[] = {57, 59, 60, 59, 57, 55}; for (int i = 0; i < 6; ++i) { music::Note n; n.start = i; n.length = 1; n.pitch = pitches[i]; n.velocity = 90; c.notes.push_back(n); }
    auto ph = sing("la-lee-loo-lee-la-loo", c, q); CHECK(ph.ok && ph.bankNotes == 6);
    for (std::size_t i = 0; i < ph.audio.frames; ++i) CHECK(std::isfinite(ph.audio.channel(0)[i]));
  }

  {  // consonants from the person's own syllables ("sah", "tah"): cut out, kept at their real level, played in place of the synthesized ones
    AudioBuffer sah, tah;
    { SingParams p = base(); p.humanize = 0; p.vibratoCents = 0; p.room = 0; auto r = sing("sa[ah]", note(57, 0.2, 2.5), p); CHECK(r.ok); sah = std::move(r.audio); }
    { SingParams p = base(); p.humanize = 0; p.vibratoCents = 0; p.room = 0; auto r = sing("ta[ah]", note(57, 0.2, 2.5), p); CHECK(r.ok); tah = std::move(r.audio); }
    const auto cs = makeConsonantEntry("s", sah, kSr), ct = makeConsonantEntry("t", tah, kSr);
    CHECK(cs.ok); CHECK(ct.ok);
    if (cs.ok) std::printf("%s\n", cs.message.c_str());
    if (cs.ok) { const double d = cs.entry.samples.size() / kSr; CHECK(d > 0.05 && d < 0.2); }
    CHECK(!makeConsonantEntry("zz", sah, kSr).ok);                                  // not a consonant
    { AudioBuffer a(1, 1000); CHECK(!makeConsonantEntry("s", a, kSr).ok); }          // too short
    { AudioBuffer a(1, static_cast<std::size_t>(kSr * 2)); CHECK(!makeConsonantEntry("s", a, kSr).ok); }   // silent
    { SingParams p = base(); p.humanize = 0; p.vibratoCents = 0; auto v = sing("ah", note(57, 0, 5), p); CHECK(v.ok); CHECK(!makeConsonantEntry("s", v.audio, kSr).ok); }  // a vowel with nothing in front of it
    VoiceBank cb = bank; if (cs.ok) cb.consonants.push_back(cs.entry); if (ct.ok) cb.consonants.push_back(ct.entry);

    // singing uses them: the output at the consonant matches the recording, and the report counts them
    SingParams q = base(); q.humanize = 0; q.vibratoCents = 0; q.bank = &cb;
    auto r = sing("sa[ah]", note(57, 0.5, 2), q); CHECK(r.ok);
    CHECK(r.bankConsonants == 1 && r.bankConsonantsSynth == 0);
    auto r2 = sing("sa[ah] ma[ah]", [&] { music::MidiClip c; for (int i = 0; i < 2; ++i) { music::Note n; n.start = 0.5 + i * 1.5; n.length = 1; n.pitch = 57; n.velocity = 90; c.notes.push_back(n); } return c; }(), q);
    CHECK(r2.ok && r2.bankConsonants == 1 && r2.bankConsonantsSynth == 1);            // "m" was not recorded: the synthesizer sings it
    if (cs.ok) {   // the stretch before the vowel is the recording (normalized correlation)
      const auto& seg = cs.entry.samples; const std::size_t n = seg.size() * 8 / 10;   // the last part is the hand-over into the vowel
      double best = 0;
      for (std::size_t off = static_cast<std::size_t>(0.05 * kSr); off < static_cast<std::size_t>(0.3 * kSr); off += 1) {   // noise needs sample-exact alignment {
        double ab = 0, aa = 0, bb = 0; for (std::size_t i = 0; i < n && off + i < r.audio.frames; ++i) { const double x = seg[i], y = r.audio.channel(0)[off + i]; ab += x * y; aa += x * x; bb += y * y; }
        if (aa > 0 && bb > 0) best = std::max(best, ab / std::sqrt(aa * bb));
      }
      std::printf("consonant correlation with the recording: %.2f\n", best);
      CHECK(best > 0.7);
    }
    {  // the consonant keeps its real loudness next to the (recorded) vowel: about the -14 dB it had in the recording
      SingParams qn = q; qn.normalize = false;
      auto n1 = sing("sa[ah]", note(57, 0.5, 2), qn);
      const double vow = rms(n1.audio, 0.4, 1.0), con = rms(n1.audio, 0.16, 0.24);
      const double below = 20 * std::log10(vow / con);
      std::printf("consonant %.1f dB below the vowel (it was 14 dB below in the recording)\n", below);
      CHECK(below > 8 && below < 22);   // a quiet fricative next to a vowel, as recorded
    }
    for (std::size_t i = 0; i < r.audio.frames; ++i) CHECK(std::isfinite(r.audio.channel(0)[i]));
    // a much shorter slot than the recording still works (consonant squeezed or cut)
    music::MidiClip fast; for (int i = 0; i < 6; ++i) { music::Note n; n.start = 0.3 + i * 0.2; n.length = 0.15; n.pitch = 57; n.velocity = 90; fast.notes.push_back(n); }
    auto f = sing("sa-ta-sa-ta-sa-ta", fast, q); CHECK(f.ok && f.bankConsonants >= 6);
    for (std::size_t i = 0; i < f.audio.frames; ++i) CHECK(std::isfinite(f.audio.channel(0)[i]));

    // file: consonants survive; an old version-1 file still loads; damage is refused
    const std::string fn = "voicebank_cons.wvb"; std::string err;
    CHECK(saveVoiceBank(fn, cb, &err));
    VoiceBank lb; CHECK(loadVoiceBank(fn, lb, &err) && lb.consonants.size() == cb.consonants.size() && lb.findConsonant("s") && lb.findConsonant("c") == lb.findConsonant("k"));
    if (lb.findConsonant("s") && cs.ok) CHECK(lb.findConsonant("s")->samples == cs.entry.samples);
    std::ifstream in(fn, std::ios::binary); std::string all((std::istreambuf_iterator<char>(in)), {});
    for (std::size_t cut : {all.size() - 5, all.size() - 4000}) { std::ofstream o("voicebank_bad.wvb", std::ios::binary); o.write(all.data(), static_cast<std::streamsize>(cut)); o.close(); VoiceBank x; CHECK(!loadVoiceBank("voicebank_bad.wvb", x, &err)); }
    VoiceBank vowelsOnly = bank; saveVoiceBank(fn, vowelsOnly, &err);
    std::ifstream in2(fn, std::ios::binary); std::string v2((std::istreambuf_iterator<char>(in2)), {});
    std::string v1 = v2.substr(0, v2.size() - 4); v1[6] = '1';   // version 1 had no consonant section
    { std::ofstream o("voicebank_v1.wvb", std::ios::binary); o.write(v1.data(), static_cast<std::streamsize>(v1.size())); }
    VoiceBank old; CHECK(loadVoiceBank("voicebank_v1.wvb", old, &err) && old.entries.size() == 3 && old.consonants.empty());
    std::remove(fn.c_str()); std::remove("voicebank_bad.wvb"); std::remove("voicebank_v1.wvb");
  }
  {  // a recording whose vowel does not match its label is flagged; correct labels are not
    int flagged = 0, falseAlarms = 0;
    for (const char* v : {"ah", "ee", "oo"}) {
      const auto good = makeBankEntry(v, held(v, 57), kSr); CHECK(good.ok); if (good.message.find("Warning") != std::string::npos) { ++falseAlarms; std::printf("false alarm: %s\n", good.message.c_str()); }
    }
    const auto bad = makeBankEntry("oo", held("ee", 57), kSr); CHECK(bad.ok && bad.message.find("closer to \"ee\"") != std::string::npos); flagged += bad.message.find("Warning") != std::string::npos;
    const auto bad2 = makeBankEntry("ee", held("ah", 57), kSr); CHECK(bad2.ok && bad2.message.find("Warning") != std::string::npos);
    CHECK(falseAlarms == 0 && flagged == 1);
    CHECK(vowelLabelWarning("ah", 0, 0).empty() && vowelLabelWarning("ah", 834, 1373).empty());
  }
  if (failures == 0) std::printf("voice bank tests passed\n");
  return failures == 0 ? 0 : 1;
}
