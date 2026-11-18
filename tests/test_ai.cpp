// Local AI layer: analysis (levels, spectrum, tempo, key, width), profiles, advice and learning,
// effects (EQ, compressor, limiter, width) and auto-master with measured results.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

#include "ai/advisor.h"
#include "ai/features.h"
#include "ai/master.h"
#include "ai/profile.h"
#include "dsp/effects.h"

using namespace wezt;
using namespace wezt::ai;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("FAIL %s:%d %s=%g vs %g\n", __FILE__, __LINE__, #a, _a, _b); ++failures; } } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846, kRate = 48000.0;

AudioBuffer sine(double hz, double db, double secs, bool stereo = true) {
  AudioBuffer b(stereo ? 2 : 1, static_cast<std::size_t>(secs * kRate));
  const double a = std::pow(10.0, db / 20.0);
  for (std::size_t i = 0; i < b.frames; ++i) for (std::size_t c = 0; c < b.channels; ++c) b.channel(c)[i] = static_cast<float>(a * std::sin(2 * kPi * hz * static_cast<double>(i) / kRate));
  return b;
}
// Deterministic noise, optionally low-passed `poles` times (darker) or differentiated (brighter).
AudioBuffer noise(double secs, double amp, int lowpasses, bool stereo = true, unsigned seed = 1) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> d(-1.f, 1.f);
  AudioBuffer b(stereo ? 2 : 1, static_cast<std::size_t>(secs * kRate));
  for (std::size_t c = 0; c < b.channels; ++c) {
    std::vector<float> x(b.frames);
    for (auto& v : x) v = d(rng);
    for (int p = 0; p < lowpasses; ++p) { float y = 0; for (auto& v : x) { y += 0.2f * (v - y); v = y; } }
    float pk = 0; for (float v : x) pk = std::max(pk, std::fabs(v));
    for (std::size_t i = 0; i < b.frames; ++i) b.channel(c)[i] = static_cast<float>(amp) * x[i] / (pk > 0 ? pk : 1.f);
  }
  return b;
}
AudioBuffer chord(std::initializer_list<double> hz, double secs) {
  AudioBuffer b(2, static_cast<std::size_t>(secs * kRate));
  for (double f : hz)
    for (int h = 1; h <= 3; ++h)
      for (std::size_t i = 0; i < b.frames; ++i) {
        const float v = static_cast<float>(0.1 / h * std::sin(2 * kPi * f * h * static_cast<double>(i) / kRate));
        b.channel(0)[i] += v; b.channel(1)[i] += v;
      }
  return b;
}
AudioBuffer clicks(double bpm, double secs) {
  AudioBuffer b(2, static_cast<std::size_t>(secs * kRate));
  const std::size_t period = static_cast<std::size_t>(kRate * 60.0 / bpm);
  for (std::size_t s = 0; s + 2000 < b.frames; s += period)
    for (std::size_t i = 0; i < 2000; ++i) { const float v = 0.8f * std::exp(-static_cast<float>(i) / 150.f) * std::sin(2.f * static_cast<float>(kPi) * 900.f * static_cast<float>(i) / static_cast<float>(kRate)); b.channel(0)[s + i] += v; b.channel(1)[s + i] += v; }
  return b;
}

void testFeatures() {
  Features f = analyze(sine(1000, -20, 6), kRate);
  CHECK(f.valid());
  NEAR(f.lufs, -20.0, 0.3);   // K-weighting adds 0.69 dB at 1 kHz, which cancels the -0.691 offset
  NEAR(f.truePeakDb, -20.0, 0.5);
  NEAR(f.crestDb, 3.0, 0.3);              // a sine's peak-to-RMS is 3 dB
  int top = 0;
  for (int b = 1; b < kBands; ++b) if (f.bandDb[static_cast<std::size_t>(b)] > f.bandDb[static_cast<std::size_t>(top)]) top = b;
  CHECK(top == 5);                        // 1 kHz band
  NEAR(f.centroidHz, 1000, 60);
  NEAR(f.stereoWidth, 0.0, 1e-4);
  NEAR(f.correlation, 1.0, 1e-4);

  AudioBuffer anti = sine(1000, -20, 3);
  for (std::size_t i = 0; i < anti.frames; ++i) anti.channel(1)[i] = -anti.channel(1)[i];
  Features fa = analyze(anti, kRate);
  NEAR(fa.stereoWidth, 1.0, 1e-3);
  NEAR(fa.correlation, -1.0, 1e-3);

  // dark vs bright noise: the spectrum must show it
  Features dark = analyze(noise(4, 0.5, 3), kRate), bright = analyze(noise(4, 0.5, 0), kRate);
  CHECK(dark.centroidHz < bright.centroidHz * 0.5f);
  CHECK(dark.bandDb[1] > dark.bandDb[8] + 6.f);
  CHECK(!analyze(AudioBuffer(), kRate).valid());
  CHECK(!analyze(AudioBuffer(2, 10), kRate).valid());
}

void testTempoAndKey() {
  for (double bpm : {120.0, 90.0, 140.0}) {
    Features f = analyze(clicks(bpm, 16), kRate);
    NEAR(f.bpm, bpm, bpm * 0.015);
    CHECK(f.bpmConfidence > 0.1f);
  }
  Features cmaj = analyze(chord({261.63, 329.63, 392.0}, 4), kRate);
  CHECK(cmaj.keyRoot == 0 && !cmaj.minor);
  Features amin = analyze(chord({220.0, 261.63, 329.63}, 4), kRate);
  CHECK(amin.keyRoot == 9 && amin.minor);
  Features gmaj = analyze(chord({196.0, 246.94, 293.66}, 4), kRate);
  CHECK(gmaj.keyRoot == 7 && !gmaj.minor);
  CHECK(keyName(9, true) == "A minor" && keyName(6, false) == "F# major" && keyName(-1, false) == "?");
  Features silent = analyze(AudioBuffer(2, 48000 * 2), kRate);
  CHECK(silent.keyRoot == -1);
}

void testProfiles() {
  namespace fs = std::filesystem;
  const std::string dir = (fs::temp_directory_path() / "wezt_profiles_test").string();
  fs::remove_all(dir);
  StyleProfile p("My Mixes");
  Features a = analyze(noise(3, 0.5, 1, true, 1), kRate), b = analyze(noise(3, 0.4, 1, true, 2), kRate);
  p.add(a); p.add(b); p.add(Features{});  // an invalid analysis is ignored
  CHECK(p.count() == 2);
  NEAR(p.mean().lufs, 0.5 * (a.lufs + b.lufs), 1e-3);
  const auto sd = p.stddev();
  NEAR(sd[0], std::fabs(a.lufs - b.lufs) / std::sqrt(2.0), 1e-3);
  ProfileStore st(dir);
  std::string err;
  CHECK(st.save(p, &err));
  CHECK(st.list().size() == 1 && st.list()[0] == "My Mixes");
  StyleProfile q;
  CHECK(st.load("My Mixes", q) && q.count() == 2);
  NEAR(q.mean().centroidHz, p.mean().centroidHz, 1e-2);
  // names cannot escape the folder
  {
    const std::string n = ProfileStore::safeName("../../etc/passwd");
    CHECK(!n.empty() && n.find('/') == std::string::npos && n.find('\\') == std::string::npos && n.find('.') == std::string::npos);
    CHECK(ProfileStore::safeName("/").empty() && !st.load("/", q) && !st.remove("/") && !st.load("..", q));
  }
  // damaged files are rejected, not half-loaded
  {
    std::ofstream f(dir + "/bad.wprofile");
    f << "WEZT-PROFILE 1\nname bad\ncount 3\nmean 1 2 3\nm2 1 2 3\nend\n";
  }
  CHECK(!st.load("bad", q));
  {
    std::ofstream f(dir + "/cut.wprofile");
    f << "WEZT-PROFILE 1\nname cut\ncount 1\n";
  }
  CHECK(!st.load("cut", q));
  CHECK(st.remove("bad") && st.remove("cut") && st.remove("My Mixes") && st.list().empty());
  // key statistics
  StyleProfile k("k");
  for (int i = 0; i < 3; ++i) k.add(analyze(chord({220.0, 261.63, 329.63}, 3), kRate));
  CHECK(k.commonKey().first == 9 && k.commonKey().second);
  fs::remove_all(dir);
}

void testAdvisorAndLearning() {
  StyleProfile bright("bright");
  for (unsigned s = 1; s <= 4; ++s) bright.add(analyze(noise(4, 0.6, 0, true, s), kRate));
  Features darkTrack = analyze(noise(4, 0.2, 3), kRate);
  Preferences pref;
  auto sug = advise(darkTrack, bright, pref);
  CHECK(!sug.empty());
  bool lufsUp = false, highBoost = false;
  for (auto& s : sug) {
    if (s.kind == SuggestionKind::Loudness && s.value > 0) lufsUp = true;
    if (s.kind == SuggestionKind::EqBand && s.band >= 7 && s.value > 0) highBoost = true;
    CHECK(!s.text.empty() && s.confidence >= 0.25f && s.confidence <= 1.f);
  }
  CHECK(lufsUp && highBoost);
  for (std::size_t i = 1; i < sug.size(); ++i) CHECK(sug[i - 1].confidence >= sug[i].confidence);
  // nothing to say when the track already matches
  Features same = bright.mean();
  CHECK(advise(same, bright, pref).empty());
  CHECK(advise(Features{}, bright, pref).empty() && advise(darkTrack, StyleProfile("empty"), pref).empty());

  // Learning: declining the same advice moves the target and lowers how often it is offered.
  Suggestion hb;
  for (auto& s : sug) if (s.kind == SuggestionKind::EqBand && s.band >= 7 && s.value > 0) { hb = s; break; }
  const float before = pref.acceptRate(hb.key());
  for (int i = 0; i < 4; ++i) learn(pref, hb, false, 0.f);
  CHECK(pref.acceptRate(hb.key()) < before);
  auto again = advise(darkTrack, bright, pref);
  float now = 0.f, was = hb.value;
  for (auto& s : again) if (s.kind == SuggestionKind::EqBand && s.band == hb.band) now = s.value;
  CHECK(now < was);
  // Accepting with a bigger value than suggested pushes later suggestions up.
  Preferences p2;
  learn(p2, hb, true, hb.value + 2.f);
  CHECK(p2.bandBias[static_cast<std::size_t>(hb.band)] > 0.f && p2.acceptRate(hb.key()) > 0.5f);
  // Preferences survive a round trip and a damaged file is refused.
  Preferences back;
  CHECK(Preferences::parse(pref.serialize(), back));
  NEAR(back.bandBias[static_cast<std::size_t>(hb.band)], pref.bandBias[static_cast<std::size_t>(hb.band)], 1e-5);
  CHECK(back.record.size() == pref.record.size());
  CHECK(!Preferences::parse("WEZT-PREFS 1\nbias nan 0 0\nend\n", back));
  CHECK(!Preferences::parse("garbage", back));
  CHECK(!Preferences::parse("WEZT-PREFS 1\nrec eq:1 5 2\nend\n", back));
}

double levelAt(Processor& p, double hz, double db = -30) {
  AudioBuffer in = sine(hz, db, 1.0), out(2, in.frames);
  p.prepare(kRate, in.frames);
  p.process(in, out);
  double s = 0; std::size_t n = 0;
  for (std::size_t i = in.frames / 2; i < in.frames; ++i) { s += out.channel(0)[i] * out.channel(0)[i]; ++n; }
  return 10 * std::log10(s / static_cast<double>(n)) + 3.0103;  // back to peak dB
}

void testEffects() {
  dsp::ParametricEq eq;
  dsp::EqBand b; b.freq = 1000; b.gainDb = 6; b.q = 1;
  eq.setBands({b});
  NEAR(levelAt(eq, 1000), -30 + 6, 0.1);
  NEAR(levelAt(eq, 100), -30, 0.3);
  NEAR(levelAt(eq, 10000), -30, 0.3);
  dsp::EqBand hs; hs.type = dsp::FilterType::HighShelf; hs.freq = 8000; hs.gainDb = -6; hs.q = 0.7f;
  eq.setBands({hs});
  NEAR(levelAt(eq, 16000), -36, 0.6);
  NEAR(levelAt(eq, 500), -30, 0.2);
  eq.setBands({});
  NEAR(levelAt(eq, 1000), -30, 0.01);  // no bands: transparent
  // coefficient response matches what the filter actually does
  const auto c = dsp::BiquadCoeffs::design(dsp::FilterType::Peaking, kRate, 2000, 5.0, 2.0);
  NEAR(c.responseDb(kRate, 2000), 5.0, 1e-6);

  // compressor: reduces level above the threshold by the ratio, leaves quiet material alone
  dsp::Compressor comp;
  dsp::CompressorParams cp; cp.thresholdDb = -20; cp.ratio = 4; cp.attackMs = 1; cp.releaseMs = 50; cp.kneeDb = 0;
  comp.set(cp);
  NEAR(levelAt(comp, 1000, -40), -40, 0.2);
  const double loud = levelAt(comp, 1000, -8);  // 12 dB over: 12*(1-1/4) = 9 dB of reduction
  NEAR(loud, -8 - 9, 0.7);
  cp.makeupDb = 5; cp.mix = 0.f; comp.set(cp);
  NEAR(levelAt(comp, 1000, -8), -8, 0.01);  // fully dry

  // limiter: never above the ceiling, quiet audio passes unchanged but delayed
  dsp::Limiter lim;
  dsp::LimiterParams lp; lp.ceilingDb = -3.f; lim.set(lp);
  AudioBuffer hot = sine(440, 6, 1.0);        // +6 dBFS: way over
  lim.prepare(kRate, 1024);
  AudioBuffer out(2, hot.frames);
  { AudioBuffer bi(2, 1024), bo(2, 1024);
    for (std::size_t pos = 0; pos < hot.frames; pos += 1024) {
      const std::size_t n = std::min<std::size_t>(1024, hot.frames - pos);
      bi.setFrames(n); bo.setFrames(n);
      for (int c = 0; c < 2; ++c) std::copy(hot.channel(c) + pos, hot.channel(c) + pos + n, bi.channel(c));
      lim.process(bi, bo);
      for (int c = 0; c < 2; ++c) std::copy(bo.channel(c), bo.channel(c) + n, out.channel(c) + pos);
    } }
  float pk = 0; for (std::size_t i = 0; i < out.frames; ++i) pk = std::max(pk, std::fabs(out.channel(0)[i]));
  CHECK(pk <= dsp::dbToGain(-3.f) + 1e-6f);
  CHECK(pk > dsp::dbToGain(-3.5f));              // and it does reach the ceiling, it is not just muted
  CHECK(lim.latencySamples() > 0);
  dsp::Limiter lq; lq.set(lp);
  NEAR(levelAt(lq, 1000, -20), -20, 0.05);

  // width
  dsp::StereoWidth w;
  AudioBuffer in(2, 100), o2(2, 100);
  for (int i = 0; i < 100; ++i) { in.channel(0)[i] = 1.f; in.channel(1)[i] = 0.f; }
  w.setWidth(0.f); w.process(in, o2);
  NEAR(o2.channel(0)[5], 0.5, 1e-6); NEAR(o2.channel(1)[5], 0.5, 1e-6);   // mono
  w.setWidth(1.f); w.process(in, o2);
  NEAR(o2.channel(0)[5], 1.0, 1e-6); NEAR(o2.channel(1)[5], 0.0, 1e-6);   // unchanged
}

void testMaster() {
  // A quiet, dark track mastered toward a loud, bright target.
  AudioBuffer src = noise(10, 0.05, 2);
  StyleProfile target("target");
  for (unsigned s = 1; s <= 3; ++s) target.add(analyze(noise(10, 0.8, 0, true, s), kRate));
  const Features before = analyze(src, kRate);
  auto sug = advise(before, target, Preferences{});
  CHECK(!sug.empty());
  MasterSettings ms = planMaster(before, sug, -14.f, -1.f);
  CHECK(!ms.eq.empty());
  MasterResult r = applyMaster(src, kRate, ms);
  CHECK(r.ok && r.audio.frames == src.frames);   // limiter latency is removed again
  NEAR(r.after.lufs, ms.targetLufs, 0.5);
  CHECK(r.after.truePeakDb <= ms.ceilingDb + 0.1f);
  // closer to the target tonally: brighter than the source
  CHECK(r.after.centroidHz > before.centroidHz);
  const float tc = target.mean().centroidHz;
  CHECK(std::fabs(r.after.centroidHz - tc) < std::fabs(before.centroidHz - tc));

  // delivery presets and a hard case: asking for very loud material must still respect the ceiling
  CHECK(deliveryTargets().size() == 5);
  MasterSettings loud; loud.targetLufs = -6.f; loud.ceilingDb = -1.f;
  MasterResult lr = applyMaster(noise(8, 0.3, 1), kRate, loud);
  CHECK(lr.ok && lr.after.truePeakDb <= -0.85f);
  // loudness suggestion maths: planMaster takes the target from f.lufs + the suggestion
  Suggestion ls; ls.kind = SuggestionKind::Loudness; ls.value = 3.f;
  Features f; f.lufs = -20.f; f.seconds = 1.0;
  NEAR(planMaster(f, {ls}, -14.f).targetLufs, -17.0, 1e-5);
  // bad input
  CHECK(!applyMaster(AudioBuffer(), kRate, loud).ok);
  CHECK(!applyMaster(AudioBuffer(2, 20), kRate, loud).ok);
}

}  // namespace

int main() {
  testFeatures();
  testTempoAndKey();
  testProfiles();
  testAdvisorAndLearning();
  testEffects();
  testMaster();
  std::printf(failures ? "FAILED\n" : "ai tests passed\n");
  return failures;
}
