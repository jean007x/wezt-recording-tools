// Sampler: pitch, zones, envelopes, loops, alternation, polyphony, filter, LFO, patches, mapping.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "instruments/sampler.h"
#include "io/wav.h"

using namespace wezt;
using namespace wezt::instruments;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("FAIL %s:%d %s=%g vs %g\n", __FILE__, __LINE__, #a, _a, _b); ++failures; } } while (0)

static constexpr double kRate = 48000.0;
struct Out { std::vector<float> l, r; };

static std::shared_ptr<AudioBuffer> mk(std::size_t ch, std::size_t n) { return std::make_shared<AudioBuffer>(ch, n); }
static std::shared_ptr<AudioBuffer> dc(float v, std::size_t n = 48000) { auto b = mk(1, n); std::fill(b->channel(0), b->channel(0) + n, v); return b; }
static std::shared_ptr<AudioBuffer> sine(double hz, std::size_t n = 48000, float amp = 1.f) {
  auto b = mk(1, n);
  for (std::size_t i = 0; i < n; ++i) b->channel(0)[i] = amp * static_cast<float>(std::sin(2 * 3.14159265358979 * hz * static_cast<double>(i) / kRate));
  return b;
}
static std::shared_ptr<AudioBuffer> ramp(std::size_t n) { auto b = mk(1, n); for (std::size_t i = 0; i < n; ++i) b->channel(0)[i] = static_cast<float>(i) / static_cast<float>(n); return b; }

static Zone zoneOf(std::shared_ptr<const AudioBuffer> a, int root = 60) { Zone z; z.audio = std::move(a); z.rootKey = root; z.sampleRate = kRate; return z; }

static SamplerParams plain() {
  SamplerParams p;
  p.amp = {0.f, 0.f, 1.f, 0.f};
  p.velocityToAmp = 0.f;
  return p;
}

static Out run(Sampler& s, std::size_t frames, std::size_t block = 64) {
  Out o; o.l.reserve(frames); o.r.reserve(frames);
  AudioBuffer in(2, block), out(2, block);
  for (std::size_t done = 0; done < frames; done += block) {
    const std::size_t n = std::min(block, frames - done);
    in.setFrames(n); out.setFrames(n);
    s.process(in, out);
    o.l.insert(o.l.end(), out.channel(0), out.channel(0) + n);
    o.r.insert(o.r.end(), out.channel(1), out.channel(1) + n);
  }
  return o;
}
static std::unique_ptr<Sampler> make(const SamplerParams& p, std::vector<Zone> zones) {
  auto s = std::make_unique<Sampler>();
  s->setParams(p);
  for (auto& z : zones) s->addZone(std::move(z));
  s->prepare(kRate, 512);
  return s;
}
static int crossings(const std::vector<float>& v, std::size_t from = 0, std::size_t to = 0) {
  if (!to) to = v.size();
  int n = 0;
  for (std::size_t i = from + 1; i < to; ++i) if ((v[i - 1] < 0) != (v[i] < 0)) ++n;
  return n;
}
static float peakOf(const std::vector<float>& v, std::size_t from = 0, std::size_t to = 0) {
  if (!to) to = v.size();
  float p = 0; for (std::size_t i = from; i < to; ++i) p = std::max(p, std::fabs(v[i])); return p;
}
static float rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double s = 0; for (std::size_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i]; return static_cast<float>(std::sqrt(s / static_cast<double>(to - from)));
}

static void testPitch() {
  Zone z = zoneOf(sine(100.0), 60);
  z.loop = LoopMode::Forward;
  auto s = make(plain(), {z});
  s->noteOn(60, 100);
  Out o = run(*s, 48000);
  NEAR(crossings(o.l), 200, 2);            // 100 Hz at the root key
  s->noteOff(60); run(*s, 256);
  s->noteOn(72, 100);
  o = run(*s, 48000);
  NEAR(crossings(o.l), 400, 3);            // one octave up
  s->allNotesOff(); run(*s, 256);
  s->noteOn(48, 100);
  o = run(*s, 48000);
  NEAR(crossings(o.l), 100, 2);            // one octave down
  // tuning in cents, bend
  s->allNotesOff(); run(*s, 256);
  SamplerParams p = plain(); p.pitchBendSemitones = 12.f;
  s->setParams(p);
  s->pitchBend(16383);
  s->noteOn(60, 100);
  o = run(*s, 48000);
  NEAR(crossings(o.l), 400, 6);            // bend of +12 semitones (almost: 16383/8192)
  // a sample recorded at another rate still plays at the right pitch
  Zone z2 = zoneOf(sine(100.0, 96000), 60); z2.sampleRate = 96000.0; z2.loop = LoopMode::Forward;
  auto s2 = make(plain(), {z2});
  s2->noteOn(60, 100);
  o = run(*s2, 48000);
  NEAR(crossings(o.l), 400, 3);  // 96 kHz data of a "100 Hz at 48k" sine = 200 Hz when played at the file's own rate
}

static void testZones() {
  Zone a = zoneOf(dc(0.25f)), b = zoneOf(dc(0.5f)), c = zoneOf(dc(0.125f));
  a.keyLo = 0; a.keyHi = 59; b.keyLo = 60; b.keyHi = 127; b.velLo = 1; b.velHi = 63;
  c.keyLo = 60; c.keyHi = 127; c.velLo = 64; c.velHi = 127;
  for (auto* z : {&a, &b, &c}) z->rootKey = z->keyLo;  // no pitch change in the test
  a.loop = b.loop = c.loop = LoopMode::Forward;
  auto s = make(plain(), {a, b, c});
  s->noteOn(30, 100); Out o = run(*s, 512);
  NEAR(o.l[500], 0.25 * std::cos(3.14159265 / 4), 1e-3);
  s->allNotesOff(); run(*s, 64);
  s->noteOn(70, 20); o = run(*s, 512);
  NEAR(o.l[500], 0.5 * std::cos(3.14159265 / 4), 1e-3);
  s->allNotesOff(); run(*s, 64);
  s->noteOn(70, 100); o = run(*s, 512);
  NEAR(o.l[500], 0.125 * std::cos(3.14159265 / 4), 1e-3);
  // outside every zone: silence, no voice
  auto s2 = make(plain(), {a});
  s2->noteOn(100, 100);
  o = run(*s2, 256);
  CHECK(peakOf(o.l) == 0.f && s2->activeVoices() == 0);
  // layers: two zones on one key add up
  Zone d1 = zoneOf(dc(0.1f)), d2 = zoneOf(dc(0.2f));
  d1.loop = d2.loop = LoopMode::Forward;
  auto s3 = make(plain(), {d1, d2});
  s3->noteOn(60, 100); o = run(*s3, 256);
  NEAR(o.l[200], 0.3 * std::cos(3.14159265 / 4), 1e-3);
}

static void testEnvelopeAndRelease() {
  SamplerParams p = plain();
  p.amp = {0.010f, 0.f, 1.f, 0.020f};
  Zone z = zoneOf(dc(1.f)); z.loop = LoopMode::Forward;
  z.pan = 0.f;
  auto s = make(p, {z});
  const float k = std::cos(3.14159265f / 4);
  s->noteOn(60, 100);
  Out o = run(*s, 2400);
  NEAR(o.l[239] / k, 0.5, 0.03);      // halfway through the 10 ms attack (frame 240 of 480)
  NEAR(o.l[600] / k, 1.0, 1e-3);      // fully open afterwards
  s->noteOff(60);
  o = run(*s, 2400);                   // release 20 ms = 960 frames
  NEAR(o.l[479] / k, 0.5, 0.05);
  CHECK(peakOf(o.l, 1100) == 0.f);
  CHECK(s->activeVoices() == 0);
  // sustain level and decay
  p.amp = {0.f, 0.010f, 0.5f, 0.f};
  s->setParams(p);
  s->noteOn(60, 100);
  o = run(*s, 2400);
  NEAR(o.l[2000] / k, 0.5, 1e-3);
  NEAR(o.l[240] / k, 0.75, 0.03);
  // zero release: a note-off cuts at once and frees the voice
  s->noteOff(60); o = run(*s, 128);
  CHECK(s->activeVoices() == 0);
}

static void testOneShotAndEnd() {
  SamplerParams p = plain(); p.amp.release = 0.5f;
  Zone z = zoneOf(dc(1.f, 4800)); z.oneShot = true;
  auto s = make(p, {z});
  s->noteOn(60, 100);
  run(*s, 480);
  s->noteOff(60);                       // ignored
  Out o = run(*s, 4000);
  CHECK(peakOf(o.l, 0, 3000) > 0.5f);   // still sounding long after the note-off
  run(*s, 2000);
  CHECK(s->activeVoices() == 0);        // ended with the sample
  // a plain non-looping zone ends by itself too
  Zone z2 = zoneOf(dc(1.f, 1000));
  auto s2 = make(plain(), {z2});
  s2->noteOn(60, 100);
  run(*s2, 2000);
  CHECK(s2->activeVoices() == 0);
}

static void testLoops() {
  // forward loop of a ramp
  auto r = ramp(1000);
  Zone z = zoneOf(r); z.loop = LoopMode::Forward; z.loopStart = 200; z.loopEnd = 400;
  auto s = make(plain(), {z});
  s->noteOn(60, 100);
  Out o = run(*s, 4800);
  CHECK(s->activeVoices() == 1);
  float mn = 1e9f, mx = -1e9f;
  for (std::size_t i = 1000; i < o.l.size(); ++i) { mn = std::min(mn, o.l[i]); mx = std::max(mx, o.l[i]); }
  const float k = std::cos(3.14159265f / 4);
  CHECK(mn >= 0.195f * k && mx <= 0.401f * k);  // stays inside the loop
  // ping-pong: rises, then falls
  Zone pz = zoneOf(r); pz.loop = LoopMode::PingPong; pz.loopStart = 200; pz.loopEnd = 400;
  auto s2 = make(plain(), {pz});
  s2->noteOn(60, 100);
  o = run(*s2, 1000);
  bool up = false, down = false;
  for (std::size_t i = 300; i < 900; ++i) { if (o.l[i] > o.l[i - 1] + 1e-6f) up = true; if (o.l[i] < o.l[i - 1] - 1e-6f) down = true; }
  CHECK(up && down);
  mn = 1e9f; mx = -1e9f;
  for (std::size_t i = 250; i < 1000; ++i) { mn = std::min(mn, o.l[i]); mx = std::max(mx, o.l[i]); }
  CHECK(mn >= 0.195f * k && mx <= 0.401f * k);
  // crossfade removes the click at the loop point
  auto sn = sine(437.0, 3000);
  Zone x = zoneOf(sn); x.loop = LoopMode::Forward; x.loopStart = 500; x.loopEnd = 2000;  // not a whole number of periods
  auto sa = make(plain(), {x});
  x.loopCrossfade = 400;
  auto sb = make(plain(), {x});
  sa->noteOn(60, 100); sb->noteOn(60, 100);
  const Out oa = run(*sa, 30000, 128), ob = run(*sb, 30000, 128);
  auto maxStep = [](const std::vector<float>& v) { float m = 0; for (std::size_t i = 2000; i < v.size(); ++i) m = std::max(m, std::fabs(v[i] - v[i - 1])); return m; };
  CHECK(maxStep(ob.l) < 0.5f * maxStep(oa.l));
  // reverse
  Zone rv = zoneOf(ramp(2000)); rv.reverse = true;
  auto s3 = make(plain(), {rv});
  s3->noteOn(60, 100); o = run(*s3, 1000);
  CHECK(o.l[0] > o.l[500] && o.l[0] > 0.9f * k * 0.99f);
}

static void testAlternation() {
  std::vector<Zone> zs;
  for (int i = 0; i < 3; ++i) { Zone z = zoneOf(dc(0.1f * static_cast<float>(i + 1))); z.loop = LoopMode::Forward; z.rrGroup = 1; z.rrIndex = i; zs.push_back(z); }
  auto s = make(plain(), zs);
  const float k = std::cos(3.14159265f / 4);
  std::vector<int> seq;
  for (int n = 0; n < 7; ++n) {
    s->noteOn(60, 100);
    const Out o = run(*s, 64);
    seq.push_back(static_cast<int>(std::lround(o.l[63] / k / 0.1f)));
    s->allNotesOff(); run(*s, 64);
  }
  CHECK((seq == std::vector<int>{1, 2, 3, 1, 2, 3, 1}));
  SamplerParams p = plain(); p.alternation = Alternation::Random;
  s->setParams(p);
  int seen[4] = {};
  for (int n = 0; n < 60; ++n) {
    s->noteOn(60, 100);
    const Out o = run(*s, 64);
    const int v = static_cast<int>(std::lround(o.l[63] / k / 0.1f));
    if (v >= 1 && v <= 3) ++seen[v];
    s->allNotesOff(); run(*s, 64);
  }
  CHECK(seen[1] > 5 && seen[2] > 5 && seen[3] > 5 && seen[1] + seen[2] + seen[3] == 60);
}

static void testPolyphonyAndPedal() {
  SamplerParams p = plain(); p.polyphony = 4;
  Zone z = zoneOf(sine(200.0)); z.loop = LoopMode::Forward; z.keyLo = 0; z.keyHi = 127;
  auto s = make(p, {z});
  for (int k = 40; k < 50; ++k) s->noteOn(k, 100);
  run(*s, 1024);
  CHECK(s->activeVoices() == 4);                 // the extra voices faded out
  // the newest notes survive
  s->allNotesOff(); run(*s, 256);
  // sustain pedal holds a released note until the pedal goes up
  p = plain(); p.amp.release = 0.01f;
  s->setParams(p);
  s->controller(64, 127);
  s->noteOn(60, 100); run(*s, 256);
  s->noteOff(60);
  Out o = run(*s, 4800);
  CHECK(peakOf(o.l, 4000) > 0.1f);
  s->controller(64, 0);
  o = run(*s, 4800);
  CHECK(peakOf(o.l, 2000) == 0.f && s->activeVoices() == 0);
  // vel 0 is a note-off; bad arguments are refused
  CHECK(!s->noteOn(200, 10) && !s->noteOff(-1) && !s->controller(300, 0));
  // queue overflow is reported, not lost silently
  int refused = 0;
  for (int i = 0; i < 5000; ++i) if (!s->noteOn(60, 100)) ++refused;
  CHECK(refused > 3000);
  run(*s, 128);
}

static void testFilterAndLfo() {
  Zone z = zoneOf(sine(10000.0, 48000)); z.loop = LoopMode::Forward;
  SamplerParams off = plain();
  auto s = make(off, {z});
  s->noteOn(60, 100);
  Out o = run(*s, 9600);
  const float open = rms(o.l, 4800, 9600);
  SamplerParams lp = plain(); lp.filter = FilterType::LowPass; lp.cutoffHz = 500.f;
  auto s2 = make(lp, {z});
  s2->noteOn(60, 100); o = run(*s2, 9600);
  CHECK(rms(o.l, 4800, 9600) < open * 0.05f);          // > 26 dB down
  SamplerParams hp = plain(); hp.filter = FilterType::HighPass; hp.cutoffHz = 500.f;
  auto s3 = make(hp, {z});
  s3->noteOn(60, 100); o = run(*s3, 9600);
  CHECK(rms(o.l, 4800, 9600) > open * 0.9f);
  // low tone: high-pass removes it
  Zone low = zoneOf(sine(100.0)); low.loop = LoopMode::Forward;
  auto s4 = make(hp, {low});
  s4->noteOn(60, 100); o = run(*s4, 9600);
  CHECK(rms(o.l, 4800, 9600) < 0.05f);
  // filter envelope opens a closed filter on note start
  SamplerParams fe = plain(); fe.filter = FilterType::LowPass; fe.cutoffHz = 200.f; fe.filterEnvOctaves = 6.f;
  fe.filterEnv = {0.f, 0.2f, 0.f, 0.1f};
  auto s5 = make(fe, {z});
  s5->noteOn(60, 100); o = run(*s5, 24000);
  CHECK(rms(o.l, 0, 600) > 5 * rms(o.l, 20000, 24000));
  // tremolo
  SamplerParams tr = plain(); tr.lfo[0].rateHz = 2.f; tr.lfo[0].amp = 1.f;
  Zone d = zoneOf(dc(1.f)); d.loop = LoopMode::Forward;
  auto s6 = make(tr, {d});
  s6->noteOn(60, 100); o = run(*s6, 48000);
  float mn = 1e9f, mx = 0.f;
  for (std::size_t i = 100; i < o.l.size(); ++i) { mn = std::min(mn, o.l[i]); mx = std::max(mx, o.l[i]); }
  CHECK(mn < 0.1f * mx && mx > 0.6f);
  // vibrato: the pitch wobbles
  SamplerParams vb = plain(); vb.lfo[0].rateHz = 4.f; vb.lfo[0].pitchCents = 400.f;
  Zone sn = zoneOf(sine(1000.0)); sn.loop = LoopMode::Forward;
  auto s7 = make(vb, {sn});
  s7->noteOn(60, 100); o = run(*s7, 48000);
  int c1 = crossings(o.l, 0, 6000), c2 = crossings(o.l, 6000, 12000);
  CHECK(c1 != c2);
  // mod wheel scales the LFO: wheel at 0 = no vibrato
  vb.lfo[0].via = LfoVia::ModWheel;
  s7->setParams(vb);
  s7->allNotesOff(); run(*s7, 128);
  s7->noteOn(60, 100); o = run(*s7, 48000);
  NEAR(crossings(o.l), 2000, 3);
}


static int crossingsOf(const std::vector<float>& v, std::size_t a, std::size_t b) {
  int c = 0;
  for (std::size_t i = a + 1; i < b; ++i) if ((v[i - 1] < 0.f) != (v[i] < 0.f)) ++c;
  return c;
}

static void testEditorFeatures() {
  Zone loopSine = zoneOf(sine(1000.0)); loopSine.loop = LoopMode::Forward;
  Zone loopDc = zoneOf(dc(1.f)); loopDc.loop = LoopMode::Forward;
  // hold stage: full level for the hold time, then the decay starts
  {
    SamplerParams p = plain(); p.amp = {0.f, 0.1f, 0.f, 0.f, 0.1f};
    auto s = make(p, {loopDc});
    s->noteOn(60, 100); Out o = run(*s, 24000);
    const double full = 0.70710678;       // centre pan of a mono sample
    NEAR(o.l[2000], full, 0.01);          // held
    NEAR(o.l[4700], full, 0.01);           // still inside the 0.1 s hold (4800 frames)
    CHECK(o.l[7200] > 0.2f && o.l[7200] < 0.5f);   // halfway down the 0.1 s decay
    NEAR(o.l[12000], 0.0, 0.01);
  }
  // key tracking off: any key plays the sample at its own pitch
  {
    SamplerParams p = plain(); p.pitchKeyTrack = false;
    auto s = make(p, {loopSine});
    s->noteOn(72, 100); Out o = run(*s, 48000);
    NEAR(crossingsOf(o.l, 0, 48000), 2000, 3);
    p.pitchKeyTrack = true; s->setParams(p); s->allNotesOff(); run(*s, 128);
    s->noteOn(72, 100); o = run(*s, 48000);
    NEAR(crossingsOf(o.l, 0, 48000), 4000, 4);
  }
  // coarse tuning
  {
    SamplerParams p = plain(); p.coarseSemitones = 12.f;
    auto s = make(p, {loopSine});
    s->noteOn(60, 100); Out o = run(*s, 48000);
    NEAR(crossingsOf(o.l, 0, 48000), 4000, 4);
  }
  // pitch envelope: starts an octave up and falls back
  {
    SamplerParams p = plain(); p.pitchEnvCents = 1200.f; p.pitchEnv = {0.f, 0.2f, 0.f, 0.1f};
    auto s = make(p, {loopSine});
    s->noteOn(60, 100); Out o = run(*s, 48000);
    const int early = crossingsOf(o.l, 0, 2400), late = crossingsOf(o.l, 40000, 42400);
    CHECK(early > late + 30);
    NEAR(late, 100, 4);                   // back at the original pitch
  }
  // drive makes a quiet sine louder and rounder
  {
    SamplerParams p = plain(); p.filter = FilterType::LowPass; p.cutoffHz = 20000.f;
    Zone q = zoneOf(sine(200.0, 48000, 0.3f)); q.loop = LoopMode::Forward;
    auto a = make(p, {q}); a->noteOn(60, 100); const Out oa = run(*a, 9600);
    p.filterDrive = 1.f;
    auto b = make(p, {q}); b->noteOn(60, 100); const Out ob = run(*b, 9600);
    CHECK(rms(ob.l, 4800, 9600) > 1.25f * rms(oa.l, 4800, 9600));
  }
  // tempo-synced LFO equals the same rate in Hz
  {
    SamplerParams a = plain(); a.lfo[0].rateHz = 2.f; a.lfo[0].amp = 1.f;
    SamplerParams b = plain(); b.lfo[0].sync = true; b.lfo[0].syncBeats = 1.f; b.lfo[0].amp = 1.f;
    auto sa = make(a, {loopDc}), sb = make(b, {loopDc});
    sb->setTempo(120.0);
    sa->noteOn(60, 100); sb->noteOn(60, 100);
    const Out oa = run(*sa, 24000), ob = run(*sb, 24000);
    double d = 0; for (std::size_t i = 0; i < oa.l.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(oa.l[i] - ob.l[i])));
    CHECK(d < 1e-3);
    sb->setTempo(60.0); sb->allNotesOff(); run(*sb, 128); sb->noteOn(60, 100);   // half the rate now
    const Out oc = run(*sb, 48000);
    int dips = 0; for (std::size_t i = 1; i + 1 < oc.l.size(); ++i) if (oc.l[i] < oc.l[i - 1] && oc.l[i] <= oc.l[i + 1] && oc.l[i] < 0.2f) ++dips;
    CHECK(dips == 1);
  }
  // second LFO and unipolar output
  {
    SamplerParams p = plain(); p.lfo[1].rateHz = 4.f; p.lfo[1].pitchCents = 400.f;
    auto s = make(p, {loopSine});
    s->noteOn(60, 100); Out o = run(*s, 48000);
    CHECK(crossingsOf(o.l, 0, 6000) != crossingsOf(o.l, 6000, 12000));
    p.lfo[1].bipolar = false; s->setParams(p); s->allNotesOff(); run(*s, 128);
    s->noteOn(60, 100); o = run(*s, 48000);
    CHECK(crossingsOf(o.l, 0, 48000) > 2150);       // only ever pushes the pitch up
    p.lfo[1].on = false; s->setParams(p); s->allNotesOff(); run(*s, 128);
    s->noteOn(60, 100); o = run(*s, 48000);
    NEAR(crossingsOf(o.l, 0, 48000), 2000, 3);       // switched off
  }
  // key trigger: restarting vs free running
  {
    auto twoNotes = [&](bool keyTrigger) {
      SamplerParams p = plain(); p.lfo[0].rateHz = 2.f; p.lfo[0].amp = 1.f; p.lfo[0].keyTrigger = keyTrigger;
      auto s = make(p, {loopDc});
      s->noteOn(60, 100); const Out a = run(*s, 2400);
      s->noteOff(60); run(*s, 3700);
      s->noteOn(60, 100); const Out b = run(*s, 2400);
      double d = 0; for (std::size_t i = 0; i < 2400; ++i) d = std::max(d, static_cast<double>(std::fabs(a.l[i] - b.l[i])));
      return d;
    };
    CHECK(twoNotes(true) < 1e-4);
    CHECK(twoNotes(false) > 0.05);
  }
  // all of it survives a patch round trip
  {
    SamplerParams p = plain();
    p.mode = SamplerMode::Slice; p.coarseSemitones = 7.f; p.pitchKeyTrack = false; p.pitchEnvCents = -300.f; p.pitchEnv.hold = 0.01f; p.filterDrive = 0.4f; p.amp.hold = 0.02f;
    p.lfo[1].sync = true; p.lfo[1].syncBeats = 0.5f; p.lfo[1].phase = 0.25f; p.lfo[1].bipolar = false; p.lfo[1].poly = false; p.lfo[1].keyTrigger = false;
    p.lfo[1].via = LfoVia::Velocity; p.lfo[1].filterOctaves = -1.5f; p.lfo[1].fadeIn = 0.3f; p.lfo[1].on = false;
    Sampler a; a.setParams(p);
    const std::string t = a.savePatch();
    Sampler b; CHECK(b.loadPatch(t, nullptr));
    CHECK(b.savePatch() == t);
    const SamplerParams& q = b.params();
    CHECK(q.mode == SamplerMode::Slice && !q.pitchKeyTrack && q.lfo[1].sync && !q.lfo[1].bipolar && !q.lfo[1].poly && !q.lfo[1].on && q.lfo[1].via == LfoVia::Velocity);
    NEAR(q.lfo[1].syncBeats, 0.5, 1e-6); NEAR(q.pitchEnv.hold, 0.01, 1e-6); NEAR(q.filterDrive, 0.4, 1e-6); NEAR(q.lfo[1].filterOctaves, -1.5, 1e-6);
  }
}

static void testVoiceBehaviour() {
  SamplerParams p = plain();
  Zone z = zoneOf(sine(100.0)); z.loop = LoopMode::Forward; z.keyLo = 0; z.keyHi = 127;
  // velocity sensitivity
  p.velocityToAmp = 1.f;
  auto s = make(p, {z});
  s->noteOn(60, 127); Out o = run(*s, 2400);
  const float full = peakOf(o.l);
  s->allNotesOff(); run(*s, 128);
  s->noteOn(60, 64); o = run(*s, 2400);
  NEAR(peakOf(o.l) / full, 64.0 / 127.0, 0.03);
  // pan
  Zone pz = z; pz.pan = -1.f;
  auto sp = make(plain(), {pz});
  sp->noteOn(60, 100); o = run(*sp, 1000);
  CHECK(peakOf(o.l) > 0.5f && peakOf(o.r) < 1e-6f);
  // mono: a second note replaces the first
  SamplerParams m = plain(); m.mono = true;
  auto sm = make(m, {z});
  sm->noteOn(60, 100); run(*sm, 256);
  sm->noteOn(64, 100); run(*sm, 1024);
  CHECK(sm->activeVoices() == 1);
  // legato: pitch changes without restarting; the sample position keeps going (no jump to 0)
  m.legato = true;
  auto sl = make(m, {z});
  sl->noteOn(60, 100); o = run(*sl, 1000);
  sl->noteOn(72, 100); o = run(*sl, 480);
  CHECK(sl->activeVoices() == 1);
  CHECK(std::fabs(o.l[0]) < 1.f);
  NEAR(crossings(o.l), 4, 2);  // 100 Hz * 2 for 10 ms: ~4 crossings
  // glide: pitch moves gradually (one octave per second)
  SamplerParams g = plain(); g.mono = true; g.glideSeconds = 1.f;
  auto sg = make(g, {z});
  sg->noteOn(48, 100); run(*sg, 4800);
  sg->noteOn(60, 100);
  o = run(*sg, 48000);
  const int early = crossings(o.l, 0, 4800), late = crossings(o.l, 43200, 48000);
  CHECK(late > early);
  // sample start/end
  Zone cut = zoneOf(ramp(1000)); cut.start = 500; cut.end = 600;
  auto sc = make(plain(), {cut});
  sc->noteOn(60, 100); o = run(*sc, 300);
  NEAR(o.l[0] / std::cos(3.14159265f / 4), 0.5, 0.01);
  CHECK(peakOf(o.l, 150) == 0.f);
  // missing sample: silent zone, no crash
  Zone none; none.keyLo = 0; none.keyHi = 127;
  auto sn = make(plain(), {none});
  sn->noteOn(60, 100); o = run(*sn, 256);
  CHECK(peakOf(o.l) == 0.f);
  // deterministic and independent of the block size (multiples of the control step)
  SamplerParams f = plain(); f.filter = FilterType::LowPass; f.cutoffHz = 2000.f; f.lfo[0].pitchCents = 50.f; f.lfo[0].rateHz = 3.f;
  auto a = make(f, {z}), b = make(f, {z});
  a->noteOn(60, 100); b->noteOn(60, 100);
  const Out oa = run(*a, 4096, 64), ob = run(*b, 4096, 512);
  bool same = true;
  for (std::size_t i = 0; i < oa.l.size(); ++i) same = same && oa.l[i] == ob.l[i];
  CHECK(same);
  // mono output buffer works
  AudioBuffer in(1, 64), out(1, 64);
  a->process(in, out);
  CHECK(out.peak() > 0.f);
}

static void testNamesAndMapping() {
  CHECK(Sampler::noteNameToKey("Piano_C#3.wav") == 61);
  CHECK(Sampler::noteNameToKey("/x/y/Strings_Eb2.wav") == 51);
  CHECK(Sampler::noteNameToKey("A-1.wav") == 21);
  CHECK(Sampler::noteNameToKey("Bass 45.wav") == 45);
  CHECK(Sampler::noteNameToKey("C3") == 60);
  CHECK(Sampler::noteNameToKey("Bass.wav") == -1);
  CHECK(Sampler::noteNameToKey("take 999.wav") == -1);
  CHECK(Sampler::noteNameToKey("") == -1);

  const fs::path dir = fs::temp_directory_path() / "wezt_sampler_map";
  fs::remove_all(dir); fs::create_directories(dir);
  auto write = [&](const std::string& name, float v) {
    io::WavWriter w; std::string err;
    CHECK(w.open((dir / name).string(), 1, 48000, io::WavSampleType::Float32, &err));
    std::vector<float> d(480, v); const float* p[1] = {d.data()};
    CHECK(w.write(p, 480)); CHECK(w.finalize(&err));
  };
  write("Pad_C2.wav", 0.1f); write("Pad_C3.wav", 0.2f); write("Pad_C4.wav", 0.3f); write("Pad_C3_b.wav", 0.25f); write("noise.wav", 0.f);
  std::vector<std::string> files;
  for (auto n : {"Pad_C4.wav", "Pad_C2.wav", "Pad_C3.wav", "Pad_C3_b.wav", "noise.wav", "missing_C5.wav"}) files.push_back((dir / n).string());
  Sampler s;
  std::string problems;
  CHECK(s.autoMap(files, &problems) == 4);
  CHECK(!problems.empty());                       // noise.wav and missing_C5.wav were reported
  const auto& zs = s.zones();
  CHECK(zs.size() == 4 && zs[0].rootKey == 48 && zs[0].keyLo == 0 && zs[0].keyHi == 54);
  CHECK(zs[1].rootKey == 60 && zs[1].keyLo == 55 && zs[1].keyHi == 66 && zs[2].keyLo == 55 && zs[2].keyHi == 66);
  CHECK(zs[1].rrGroup != 0 && zs[1].rrGroup == zs[2].rrGroup && zs[1].rrIndex != zs[2].rrIndex);
  CHECK(zs[3].rootKey == 72 && zs[3].keyLo == 67 && zs[3].keyHi == 127);

  // slicing
  auto a = mk(1, 48000);
  for (auto at : {0, 14400, 33600}) for (int i = 0; i < 400; ++i) a->channel(0)[at + i] = (i % 2 ? 1.f : -1.f) * std::exp(-static_cast<float>(i) / 100.f);
  Sampler t;
  CHECK(t.sliceEqual(a, kRate, 4, 36) == 4);
  CHECK(t.zones().size() == 4 && t.zones()[1].start == 12000 && t.zones()[3].end == 48000 && t.zones()[2].keyLo == 38 && t.zones()[2].oneShot);
  Sampler u;
  CHECK(u.sliceTransients(a, kRate, 0.5f, 64, 36) == 3);
  CHECK(std::llabs(u.zones()[1].start - 14400) <= 480 && std::llabs(u.zones()[2].start - 33600) <= 480 && u.zones()[0].start == 0);
  CHECK(u.zones()[2].end == 48000 && u.zones()[1].end == u.zones()[2].start);
  Sampler v;
  CHECK(v.sliceTransients(mk(1, 1000), kRate) == 1);  // silence: one slice
  CHECK(v.sliceEqual(nullptr, kRate, 4) == 0);
}

static void testPatch() {
  const fs::path dir = fs::temp_directory_path() / "wezt_sampler_patch";
  fs::remove_all(dir); fs::create_directories(dir);
  const std::string wav = (dir / "my sample.wav").string();  // a space in the name on purpose
  {
    io::WavWriter w; std::string err;
    CHECK(w.open(wav, 1, 48000, io::WavSampleType::Float32, &err));
    auto b = sine(300.0, 4800); const float* p[1] = {b->channel(0)};
    CHECK(w.write(p, 4800)); CHECK(w.finalize(&err));
  }
  Sampler a;
  Zone z; std::string err;
  CHECK(a.loadSampleFile(wav, z, &err));
  z.rootKey = 57; z.keyLo = 40; z.keyHi = 80; z.velLo = 10; z.velHi = 120; z.tuneCents = -12.5f; z.gainDb = -3.f; z.pan = 0.25f;
  z.loop = LoopMode::PingPong; z.loopStart = 100; z.loopEnd = 2000; z.loopCrossfade = 64; z.reverse = false; z.oneShot = true; z.rrGroup = 3; z.rrIndex = 1;
  a.addZone(z);
  SamplerParams p; p.polyphony = 12; p.mono = true; p.glideSeconds = 0.125f; p.amp = {0.01f, 0.2f, 0.6f, 0.4f};
  p.filter = FilterType::BandPass; p.cutoffHz = 1234.5f; p.resonance = 0.3f; p.lfo[0].shape = LfoShape::SawDown; p.alternation = Alternation::Random;
  p.lfo[0].via = LfoVia::ModWheel; p.masterGainDb = -2.f;
  a.setParams(p);
  const std::string text = a.savePatch();
  Sampler b;
  std::string warn;
  CHECK(b.loadPatch(text, &warn) && warn.empty());
  CHECK(b.savePatch() == text);                 // exact round trip
  CHECK(b.zones().size() == 1 && b.zones()[0].audio && b.zones()[0].audio->frames == 4800);
  CHECK(b.params().filter == FilterType::BandPass && b.params().alternation == Alternation::Random && b.params().mono);
  // missing sample is reported, zone stays so a later save keeps it
  fs::remove(wav);
  Sampler c;
  CHECK(c.loadPatch(text, &warn) && !warn.empty());
  CHECK(c.zones().size() == 1 && !c.zones()[0].audio && c.savePatch() == text);
  // garbage is refused and leaves the instrument untouched
  CHECK(!c.loadPatch("hello", &warn) && !c.loadPatch("WEZT-SAMPLER 1\nbogus line\n", &warn));
  CHECK(c.zones().size() == 1);
}

static void testEditWhileRunning() {
  // The audio thread keeps rendering while the UI thread adds, edits and removes zones, changes
  // parameters and plays notes. Run under ThreadSanitizer / AddressSanitizer this must stay clean.
  Zone z = zoneOf(sine(200.0, 4800)); z.loop = LoopMode::Forward; z.keyLo = 0; z.keyHi = 127;
  auto s = make(plain(), {z});
  std::atomic<bool> stop{false};
  std::atomic<long> blocks{0};
  std::thread audio([&] {
    AudioBuffer in(2, 128), out(2, 128);
    while (!stop.load()) { s->process(in, out); blocks.fetch_add(1); }
  });
  for (int i = 0; i < 400; ++i) {
    s->noteOn(40 + i % 40, 100);
    Zone n = zoneOf(sine(100.0 + i, 2400 + static_cast<std::size_t>(i)));
    n.loop = LoopMode::PingPong; n.keyLo = 0; n.keyHi = 127;
    const int k = s->addZone(n);
    n.end = 1000; n.loopEnd = 800;
    s->updateZone(static_cast<std::size_t>(k), n);
    SamplerParams p = plain(); p.filter = FilterType::LowPass; p.cutoffHz = 500.f + static_cast<float>(i);
    s->setParams(p);
    if (i % 3 == 0) s->removeZone(static_cast<std::size_t>(k));
    if (i % 50 == 0) s->noteOff(40 + i % 40);
    std::this_thread::yield();
  }
  const long before = blocks.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  stop = true;
  audio.join();
  CHECK(blocks.load() > before);                 // never got stuck behind an edit
  CHECK(s->zones().size() > 100);
  // as an insert it adds to its input
  Sampler t;
  Zone d = zoneOf(dc(0.25f)); d.loop = LoopMode::Forward;
  t.setParams(plain()); t.addZone(d); t.prepare(kRate, 64);
  AudioBuffer in(2, 64), out(2, 64);
  std::fill(in.channel(0), in.channel(0) + 64, 0.5f);
  t.noteOn(60, 100);
  t.process(in, out);
  NEAR(out.channel(0)[63], 0.5 + 0.25 * std::cos(3.14159265 / 4), 1e-3);
  NEAR(out.channel(1)[63], 0.25 * std::sin(3.14159265 / 4), 1e-3);
  // built-in descriptor and state round trip through the plugin interface
  CHECK(Sampler::descriptor().format == plugins::Format::Builtin);
  Sampler u;
  CHECK(u.loadState(t.saveState()) && u.zones().size() == 1);
}

int main() {
  testPitch();
  testZones();
  testEnvelopeAndRelease();
  testOneShotAndEnd();
  testLoops();
  testAlternation();
  testPolyphonyAndPedal();
  testFilterAndLfo();
  testEditorFeatures();
  testVoiceBehaviour();
  testNamesAndMapping();
  testPatch();
  testEditWhileRunning();
  std::printf(failures ? "FAILED\n" : "sampler tests passed\n");
  return failures;
}
