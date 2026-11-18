// The humanized singing voice: what makes it differ from a machine, checked with numbers.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "music/audio_to_midi.h"
#include "voice/singing.h"
#include "voice/voice_analysis.h"

using namespace wezt;
using namespace wezt::voice;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {
constexpr double kSr = 44100.0;

music::MidiClip oneNote(int pitch, double startBeat, double lenBeats, int velocity = 90) {
  music::MidiClip c; music::Note n; n.start = startBeat; n.length = lenBeats; n.pitch = pitch; n.velocity = velocity; c.notes.push_back(n);
  return c;
}
SingParams base() { SingParams p; p.bpm = 120; p.sampleRate = kSr; p.fitRange = false; p.voice = VoiceType::Tenor; p.room = 0; return p; }
AudioBuffer sang(const std::string& lyr, const music::MidiClip& c, const SingParams& p) { auto r = sing(lyr, c, p); CHECK(r.ok); return std::move(r.audio); }
double rms(const AudioBuffer& a, double s0, double s1) {
  double s = 0; std::size_t n = 0;
  for (std::size_t i = static_cast<std::size_t>(s0 * kSr); i < static_cast<std::size_t>(s1 * kSr) && i < a.frames; ++i, ++n) s += static_cast<double>(a.channel(0)[i]) * a.channel(0)[i];
  return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, n)));
}
double centsOf(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }
// median pitch (Hz) of the confident tracker frames in [t0, t1) seconds
double pitchIn(const AudioBuffer& a, double t0, double t1) {
  const auto fr = music::trackPitch(a, kSr);
  std::vector<double> hz;
  for (std::size_t i = 0; i < fr.size(); ++i) if (fr[i].time >= t0 && fr[i].time < t1 && fr[i].hz > 0 && fr[i].confidence > 0.4) hz.push_back(fr[i].hz);
  if (hz.empty()) return 0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}
}  // namespace

int main() {
  const double f220 = 220.0;

  {  // same seed, same performance; another seed, another performance
    const auto c = oneNote(57, 0, 3);
    auto p = base();
    const auto a = sang("la[ah]", c, p), b = sang("la[ah]", c, p);
    CHECK(a.frames == b.frames);
    bool same = a.frames == b.frames;
    for (std::size_t i = 0; same && i < a.frames; ++i) same = a.channel(0)[i] == b.channel(0)[i];
    CHECK(same);
    p.seed = 7;
    const auto d = sang("la[ah]", c, p);
    double diff = 0; for (std::size_t i = 0; i < std::min(a.frames, d.frames); ++i) diff += std::fabs(a.channel(0)[i] - d.channel(0)[i]);
    CHECK(diff > 1.0);
  }

  {  // measured against published values for sung vowels (tenor range, where the measures are reliable)
    const auto c = oneNote(57, 0, 4);
    const auto a = sang("la[ah]", c, base());
    AudioBuffer mono(1, a.frames); for (std::size_t i = 0; i < a.frames; ++i) mono.channel(0)[i] = a.channel(0)[i];
    const auto m = analyzeVoice(mono, kSr, 0.8, 1.9);
    CHECK(m.ok);
    std::printf("tenor: vibrato %.2f Hz %.0f cents, jitter %.2f %%, shimmer %.2f %%, HNR %.1f dB\n", m.vibratoRateHz, m.vibratoExtentCents, m.jitterLocalPct, m.shimmerLocalPct, m.hnrDb);
    CHECK(m.vibratoRateHz > 5.0 && m.vibratoRateHz < 7.0);              // sung vibrato: about 5-7 Hz
    CHECK(m.vibratoExtentCents > 40 && m.vibratoExtentCents < 130);     // pop to light classical: tens of cents up to ~100 peak to peak
    CHECK(m.jitterLocalPct > 0.1 && m.jitterLocalPct < 1.0);            // alive (not a machine's 0) but under the usual 1 % limit
    CHECK(m.shimmerLocalPct > 0.3 && m.shimmerLocalPct < 3.8);
    CHECK(m.hnrDb > 18);
    CHECK(m.vibratoStrength < 0.95);                                    // not a perfect sine
  }

  {  // a machine (humanize 0, no vibrato) has none of that life
    auto p = base(); p.humanize = 0; p.vibratoCents = 0; p.breath = 0;
    const auto a = sang("la[ah]", oneNote(57, 0, 4), p);
    AudioBuffer mono(1, a.frames); for (std::size_t i = 0; i < a.frames; ++i) mono.channel(0)[i] = a.channel(0)[i];
    const auto m = analyzeVoice(mono, kSr, 0.8, 1.9);
    CHECK(m.ok && m.jitterLocalPct < 0.6 && m.vibratoExtentCents < 12);
  }

  {  // no DC offset: the source is the derivative of the airflow, so it carries none (the old one did, and low voices could not even be measured)
    for (int pitch : {45, 57, 64, 72}) {
      auto p = base(); p.voice = pitch == 45 ? VoiceType::Bass : pitch == 57 ? VoiceType::Tenor : pitch == 64 ? VoiceType::Alto : VoiceType::Soprano;
      const auto a = sang("la[ah]", oneNote(pitch, 0, 4), p);
      double mean = 0, pk = 0; for (std::size_t i = 0; i < a.frames; ++i) { mean += a.channel(0)[i]; pk = std::max<double>(pk, std::fabs(a.channel(0)[i])); }
      mean /= static_cast<double>(a.frames);
      CHECK(std::fabs(mean) < 0.01 * pk);
    }
  }

  {  // vibrato starts late and grows
    auto p = base();
    const auto a = sang("la[ah]", oneNote(57, 2, 4), p);   // the note starts at 1.0 s
    AudioBuffer mono(1, a.frames); for (std::size_t i = 0; i < a.frames; ++i) mono.channel(0)[i] = a.channel(0)[i];
    const auto env = vibratoEnvelope(mono, kSr, 1.05, 2.9, 0.3);
    std::printf("vibrato envelope: "); for (double e : env) std::printf("%.0f ", e); std::printf("\n");
    CHECK(env.size() >= 4);
    if (env.size() >= 4) {
      CHECK(env[0] < 0.5 * env[3]);     // the first 0.3 s of the note: little vibrato
      CHECK(env[3] > 40);               // and it is there later
    }
  }

  {  // a note after a rest scoops up into the pitch; a machine does not
    auto p = base(); p.vibratoCents = 0; p.humanize = 0.9;
    const auto h = sang("a[ah]", oneNote(57, 2, 3), p);
    p.humanize = 0;
    const auto m = sang("a[ah]", oneNote(57, 2, 3), p);
    const double early = centsOf(pitchIn(h, 1.03, 1.08), f220), late = centsOf(pitchIn(h, 1.7, 2.3), f220);
    const double earlyM = centsOf(pitchIn(m, 1.03, 1.08), f220);
    std::printf("scoop: early %.1f cents, late %.1f cents; machine early %.1f\n", early, late, earlyM);
    CHECK(early < late - 6);
    CHECK(std::fabs(late) < 25);        // it arrives in tune
    CHECK(std::fabs(earlyM) < 6);
  }

  {  // louder is brighter; trained singers have a ring near 3 kHz
    auto p = base(); p.vibratoCents = 0;
    auto alpha = [&](int vel) {
      const auto a = sang("la[ah]", oneNote(57, 0, 4, vel), p);
      AudioBuffer mono(1, a.frames); for (std::size_t i = 0; i < a.frames; ++i) mono.channel(0)[i] = a.channel(0)[i];
      return analyzeVoice(mono, kSr, 0.8, 1.9).alphaRatioDb;
    };
    const double soft = alpha(45), loud = alpha(125);
    std::printf("alpha ratio: soft %.1f, loud %.1f dB\n", soft, loud);
    CHECK(loud > soft + 0.5);
    // the singer's formant is a narrow ring, so look at its band (2.4-3.6 kHz) against the band below it, with a steady voice so the harmonics sit on the probe bins
    auto ringBand = [&](double ringDb) {
      auto q = p; q.humanize = 0; q.singerFormantDb = ringDb;
      const auto a = sang("la[ah]", oneNote(57, 0, 4), q);
      auto band = [&](double f0, double f1) {
        double e = 0; const std::size_t n0 = static_cast<std::size_t>(0.8 * kSr), n1 = static_cast<std::size_t>(1.6 * kSr);
        for (double f = f0; f < f1; f += 10.0) {
          double re = 0, im = 0;
          for (std::size_t i = n0; i < n1; ++i) { const double w = 0.5 * (1 - std::cos(2 * M_PI * static_cast<double>(i - n0) / static_cast<double>(n1 - n0))), ph = 2 * M_PI * f * static_cast<double>(i) / kSr; re += w * a.channel(0)[i] * std::cos(ph); im += w * a.channel(0)[i] * std::sin(ph); }
          e += re * re + im * im;
        }
        return 10.0 * std::log10(e + 1e-30);
      };
      return band(2400, 3600) - band(1000, 2400);
    };
    const double flat = ringBand(0), ringy = ringBand(9);
    std::printf("2.4-3.6 kHz relative to 1-2.4 kHz: no ring %.1f dB, ring %.1f dB\n", flat, ringy);
    CHECK(ringy > flat + 3.0);
  }

  {  // a small room leaves a tail and nothing blows up
    auto p = base();
    const auto dry = sang("la[ah]", oneNote(57, 0, 2), p);
    p.room = 0.6;
    const auto wet = sang("la[ah]", oneNote(57, 0, 2), p);
    CHECK(rms(wet, 1.15, 1.45) > 3.0 * rms(dry, 1.15, 1.45));
    for (std::size_t i = 0; i < wet.frames; ++i) if (!std::isfinite(wet.channel(0)[i])) { CHECK(false); break; }
  }

  {  // timing: with humanize 0 every seed starts the vowel at the same instant; with it, they differ a few milliseconds
    auto onset = [&](double H, std::uint64_t seed) {
      auto p = base(); p.humanize = H; p.seed = seed; p.vibratoCents = 0;
      const auto a = sang("a[ah]", oneNote(57, 2, 2), p);
      double pk = 0; for (std::size_t i = 0; i < a.frames; ++i) pk = std::max<double>(pk, std::fabs(a.channel(0)[i]));
      for (std::size_t i = 0; i < a.frames; ++i) if (std::fabs(a.channel(0)[i]) > 0.2 * pk) return static_cast<double>(i) / kSr;
      return -1.0;
    };
    const double m1 = onset(0, 1), m2 = onset(0, 9);
    CHECK(std::fabs(m1 - m2) < 0.002);
    double lo = 1e9, hi = -1e9;
    for (std::uint64_t s = 1; s <= 8; ++s) { const double t = onset(0.8, s); lo = std::min(lo, t); hi = std::max(hi, t); }
    std::printf("onset spread with humanize: %.1f ms\n", (hi - lo) * 1000.0);
    CHECK(hi - lo > 0.003 && hi - lo < 0.15);
  }

  {  // a choir is made of different people, not copies
    auto p = base();
    const auto solo = sang("la[ah]", oneNote(57, 0, 3), p);
    p.choirVoices = 6;
    const auto choir = sang("la[ah]", oneNote(57, 0, 3), p);
    double dot = 0, a2 = 0, b2 = 0;
    for (std::size_t i = 0; i < std::min(solo.frames, choir.frames); ++i) { const double x = solo.channel(0)[i], y = choir.channel(0)[i]; dot += x * y; a2 += x * x; b2 += y * y; }
    CHECK(std::fabs(dot) / std::sqrt(a2 * b2) < 0.5);
    const auto choir2 = [&] { auto q = p; q.seed = 5; return sang("la[ah]", oneNote(57, 0, 3), q); }();
    double diff = 0; for (std::size_t i = 0; i < std::min(choir.frames, choir2.frames); ++i) diff += std::fabs(choir.channel(0)[i] - choir2.channel(0)[i]);
    CHECK(diff > 1.0);
  }

  {  // characters: different kinds of people, all valid and audibly different
    CHECK(characterNames().size() >= 10);
    { SingParams q; CHECK(!applyCharacter("not a voice", q)); CHECK(applyCharacter("Gritty Rock", q)); CHECK(q.roughness > 0.3); }
    auto hf = [&](const char* name) {   // share of energy in the sample-to-sample difference = brightness
      SingParams q = base(); CHECK(applyCharacter(name, q));
      const auto a = sang("la[ah]", oneNote(q.voice == VoiceType::Bass ? 45 : 60, 0, 3), q);
      double e = 0, d = 0; for (std::size_t i = 1; i < a.frames; ++i) { const double x = a.channel(0)[i], y = a.channel(0)[i - 1]; e += x * x; d += (x - y) * (x - y); if (!std::isfinite(x)) CHECK(false); }
      CHECK(e > 0); return d / e;
    };
    const double warm = hf("warm_alto"), bright = hf("bright_pop");
    std::printf("brightness warm %.3f bright %.3f\n", warm, bright);
    CHECK(bright > warm * 1.15);
    for (const auto& n : characterNames()) (void)hf(n.c_str());
    // a child-sized vocal tract moves the formants up: same note, same vowel, more energy in the difference signal than a bass
    CHECK(hf("child") > hf("deep_bass"));
  }

  if (failures == 0) std::printf("humanized singing tests passed\n");
  return failures == 0 ? 0 : 1;
}
