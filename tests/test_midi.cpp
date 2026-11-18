// Live MIDI: sample-accurate events into instruments, the track sequencer, stop/locate behaviour,
// the click-a-key preview queue, and the instruments' parameter surface.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "audio/audio_device.h"
#include "instruments/sampler.h"
#include "instruments/wezt_synth.h"
#include "session/midi_player.h"
#include "session/session.h"

using namespace wezt;
using namespace wezt::session;
using wezt::instruments::Sampler;
using wezt::instruments::WeztSynth;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

constexpr double kRate = 48000.0;

int paramId(const plugins::IPluginInstance& p, const std::string& name) {
  for (const auto& i : p.parameters()) if (i.name == name) return static_cast<int>(i.id);
  return -1;
}

void run(Processor& p, AudioBuffer& out, std::size_t frames) {
  AudioBuffer in(2, frames);
  out = AudioBuffer(2, frames);
  p.process(in, out);
}

// Sine-only synth voice so the pitch can be read from zero crossings.
void makeSine(WeztSynth& s) {
  s.setParameterNormalized(static_cast<unsigned>(paramId(s, "Saw")), 0);
  s.setParameterNormalized(static_cast<unsigned>(paramId(s, "Triangle")), 0);
  s.setParameterNormalized(static_cast<unsigned>(paramId(s, "Sine")), 1);
  s.setParameterNormalized(static_cast<unsigned>(paramId(s, "Cutoff")), 1);
  s.setParameterNormalized(static_cast<unsigned>(paramId(s, "Sustain")), 1);
}

std::size_t firstNonZero(const std::vector<float>& v, float eps = 0.f) {
  for (std::size_t i = 0; i < v.size(); ++i) if (std::fabs(v[i]) > eps) return i;
  return v.size();
}

// Engine rig, as in the session tests.
struct Rig {
  Session& s;
  std::unique_ptr<Engine> e;
  audio::NullDevice dev;
  explicit Rig(Session& session) : s(session) {
    e = std::make_unique<Engine>(s.graph(), s.maxBlock(), s.channels(), s.sampleRate(), 2);
    s.attach(e->context());
    audio::AudioConfig c;
    c.sampleRate = static_cast<unsigned>(s.sampleRate());
    c.inChannels = 0; c.outChannels = 2;
    std::string err;
    CHECK(dev.open(c, e.get(), &err) && dev.start(&err));
  }
  // Runs `total` frames in callbacks of varying size; returns the left channel.
  std::vector<float> run(std::size_t total) {
    static const unsigned sizes[] = {64, 100, 37, 128, 251, 17};
    std::vector<float> l;
    std::size_t k = 0;
    while (l.size() < total) {
      const unsigned n = static_cast<unsigned>(std::min<std::size_t>(sizes[k++ % 6], total - l.size()));
      std::vector<float> a(n), b(n);
      float* op[2] = {a.data(), b.data()};
      e->process(nullptr, op, n);
      l.insert(l.end(), a.begin(), a.end());
    }
    return l;
  }
};

float peak(const std::vector<float>& v, std::size_t from, std::size_t to) {
  float m = 0;
  for (std::size_t i = from; i < std::min(to, v.size()); ++i) m = std::max(m, std::fabs(v[i]));
  return m;
}

// ---------------------------------------------------------------------------------------------
void testSynthSampleAccurate() {
  WeztSynth s;
  s.prepare(kRate, 256);
  MidiEvent on; on.type = MidiEvent::NoteOn; on.a = 69; on.b = 100; on.offset = 100;
  s.handleMidi(&on, 1);
  AudioBuffer out;
  run(s, out, 256);
  bool silentBefore = true;
  for (int i = 0; i < 100; ++i) if (out.channel(0)[i] != 0.f || out.channel(1)[i] != 0.f) silentBefore = false;
  CHECK(silentBefore);                                                  // nothing before the event's sample
  CHECK(peak(std::vector<float>(out.channel(0), out.channel(0) + 256), 100, 256) > 0.001f);  // sound after it
  CHECK(s.activeVoices() == 1);
}

void testSynthPitchAndRelease() {
  WeztSynth s;
  s.prepare(kRate, 512);
  makeSine(s);
  MidiEvent on; on.type = MidiEvent::NoteOn; on.a = 69; on.b = 110;  // A4
  s.handleMidi(&on, 1);
  std::vector<float> l;
  AudioBuffer out;
  for (int b = 0; b < 94; ++b) { run(s, out, 512); l.insert(l.end(), out.channel(0), out.channel(0) + 512); }  // ~1 s
  int crossings = 0;
  for (std::size_t i = 4800; i + 1 < l.size(); ++i) if (l[i] <= 0.f && l[i + 1] > 0.f) ++crossings;
  const double hz = crossings / ((static_cast<double>(l.size()) - 4800) / kRate);
  CHECK(std::fabs(hz - 440.0) < 3.0);
  MidiEvent off; off.type = MidiEvent::NoteOff; off.a = 69;
  s.handleMidi(&off, 1);
  for (int b = 0; b < 60; ++b) run(s, out, 512);  // far longer than the release
  CHECK(s.activeVoices() == 0);
  CHECK(peak(std::vector<float>(out.channel(0), out.channel(0) + 512), 0, 512) < 1e-6f);
}

void testSynthDrumsAndVelocity() {
  auto hit = [](int vel) {
    WeztSynth s;
    s.setPreset("drums");
    s.prepare(kRate, 512);
    MidiEvent on; on.type = MidiEvent::NoteOn; on.a = 38; on.b = static_cast<std::int16_t>(vel);
    s.handleMidi(&on, 1);
    double e = 0;
    AudioBuffer out;
    for (int b = 0; b < 20; ++b) { run(s, out, 512); for (std::size_t i = 0; i < 512; ++i) e += out.channel(0)[i] * out.channel(0)[i]; }
    return e;
  };
  const double loud = hit(127), soft = hit(40);
  CHECK(loud > 1e-4);
  CHECK(soft > 0 && loud > soft * 3);  // velocity scales the hit
  WeztSynth s;
  s.setPreset("drums");
  s.prepare(kRate, 512);
  MidiEvent on; on.type = MidiEvent::NoteOn; on.a = 12; on.b = 100;  // not a drum: silent, no crash
  s.handleMidi(&on, 1);
  AudioBuffer out; run(s, out, 512);
  CHECK(peak(std::vector<float>(out.channel(0), out.channel(0) + 512), 0, 512) == 0.f);
}

void testSamplerSampleAccurate() {
  Sampler sm;
  auto dc = std::make_shared<AudioBuffer>(1, 4800);
  std::fill(dc->channel(0), dc->channel(0) + 4800, 1.f);
  instruments::Zone z; z.audio = dc; z.rootKey = 60; z.sampleRate = kRate; z.loop = instruments::LoopMode::Forward;
  sm.addZone(std::move(z));
  sm.prepare(kRate, 256);
  MidiEvent ev[2];
  ev[0].type = MidiEvent::NoteOn; ev[0].a = 60; ev[0].b = 127; ev[0].offset = 50;
  ev[1].type = MidiEvent::NoteOff; ev[1].a = 60; ev[1].offset = 150;  // sent out of order on purpose below
  MidiEvent rev[2] = {ev[1], ev[0]};
  sm.handleMidi(rev, 2);
  AudioBuffer out; run(sm, out, 256);
  std::vector<float> l(out.channel(0), out.channel(0) + 256);
  CHECK(firstNonZero(l) >= 50 && firstNonZero(l) <= 53);
  CHECK(peak(l, 60, 150) > 0.05f);
  for (int b = 0; b < 40; ++b) run(sm, out, 256);
  CHECK(sm.activeVoices() == 0);                                      // the note-off at 150 released it
}

void testSequenceBuild() {
  music::MidiClip c;
  c.notes.push_back({60, 0.0, 1.0, 100});
  c.notes.push_back({60, 1.0, 1.0, 90});     // same key, starts where the first ends
  c.notes.push_back({61, 0.0, 0.0, 90});     // zero length: dropped
  c.notes.push_back({200, 0.0, 1.0, 90});    // not a MIDI key: dropped
  c.notes.push_back({62, std::nan(""), 1.0, 90});
  auto seq = buildMidiSequence(c, 120.0, kRate);
  CHECK(seq->events.size() == 4);
  // at frame 24000 (beat 1) the note-off must come before the note-on
  std::vector<SeqEvent> at;
  for (auto& e : seq->events) if (e.frame == 24000) at.push_back(e);
  CHECK(at.size() == 2 && at[0].ev.type == MidiEvent::NoteOff && at[1].ev.type == MidiEvent::NoteOn);
  // windows tile the timeline: every event is seen exactly once
  std::size_t seen = 0;
  MidiEvent buf[8];
  for (std::int64_t t = 0; t < 60000; t += 777) seen += collectMidi(*seq, t, t + 777, buf, 8);
  CHECK(seen == 4);
  MidiEvent b2[8];
  const auto n = collectMidi(*seq, 23990, 24010, b2, 8);
  CHECK(n == 2 && b2[0].offset == 10 && b2[1].offset == 10);
  unsigned dropped = 0;
  CHECK(collectMidi(*seq, 0, 100000, b2, 1, &dropped) == 1 && dropped == 3);
}

// A session with one track playing a note on beat 1 (0.5 s at 120 bpm), 1 beat long.
struct Song {
  Session s{kRate, 2, 256};
  TrackId t = -1;
  WeztSynth* synth = nullptr;
  Song(double noteBeats = 1.0) {
    t = s.addTrack("Keys");
    auto syn = std::make_unique<WeztSynth>();
    synth = syn.get();
    makeSine(*synth);
    CHECK(s.addInsert(t, std::move(syn), WeztSynth::descriptor()));
    music::MidiClip c;
    c.notes.push_back({69, 1.0, noteBeats, 100});
    s.track(t)->source->setMidiSequence(buildMidiSequence(c, 120.0, kRate));
  }
};

void testSequencerThroughSession() {
  Song song;
  Rig r(song.s);
  r.e->transport().play();
  const auto l = r.run(3 * 48000);
  const std::size_t first = firstNonZero(l, 1e-9f);
  CHECK(first >= 24000 && first <= 24003);                      // beat 1 at 120 bpm = frame 24000, whatever the block sizes
  CHECK(peak(l, 30000, 47000) > 0.05f);                         // sounding while held
  CHECK(peak(l, 24000 + 24000 + 20000, l.size()) < 1e-5f);      // released after the note ended (+ release time)
  CHECK(song.synth->activeVoices() == 0);
}

void testStopAndLocateReleaseNotes() {
  {  // stop mid-note
    Song song(8.0);
    Rig r(song.s);
    r.e->transport().play();
    auto l = r.run(36000);                                       // 0.75 s: the note is sounding
    CHECK(peak(l, 30000, 36000) > 0.05f);
    r.e->transport().stop();
    l = r.run(48000);
    CHECK(peak(l, 30000, l.size()) < 1e-5f);                     // released and faded out
    CHECK(song.synth->activeVoices() == 0);
  }
  {  // the playhead jumps away from the note
    Song song(8.0);
    Rig r(song.s);
    r.e->transport().play();
    r.run(36000);
    r.e->transport().locate(480000);                             // far past the end of the sequence
    auto l = r.run(48000);
    CHECK(peak(l, 30000, l.size()) < 1e-5f);
    CHECK(song.synth->activeVoices() == 0);
  }
  {  // replacing the sequence while playing re-syncs without a stuck note
    Song song(8.0);
    Rig r(song.s);
    r.e->transport().play();
    r.run(36000);
    song.s.track(song.t)->source->setMidiSequence(buildMidiSequence(music::MidiClip{}, 120.0, kRate));
    auto l = r.run(48000);
    CHECK(peak(l, 20000, l.size()) < 1e-5f);
  }
}

void testKeyPreview() {
  Song song;
  Rig r(song.s);                                                  // transport stays stopped
  auto* src = song.s.track(song.t)->source;
  auto l = r.run(4800);
  CHECK(peak(l, 0, l.size()) == 0.f);
  CHECK(src->auditionNote(69, 100, true));
  l = r.run(9600);
  CHECK(peak(l, 2400, l.size()) > 0.05f);                         // heard with the transport stopped
  CHECK(src->auditionNote(69, 0, false));
  l = r.run(48000);
  CHECK(peak(l, 30000, l.size()) < 1e-5f);                        // key up: released
  CHECK(!src->auditionNote(200, 100, true));                      // not a key
  for (int i = 0; i < 400; ++i) src->auditionNote(60, 90, true);  // a flood fills the queue; it does not crash or block
  src->auditionAllOff();
  r.run(48000);
  l = r.run(9600);
  CHECK(peak(l, 0, l.size()) < 1e-5f);
}

void testKeyboardControllers() {
  {  // sustain pedal holds a released key; lifting it releases
    Song song; Rig r(song.s); auto* src = song.s.track(song.t)->source;
    CHECK(src->auditionController(64, 127));
    CHECK(src->auditionNote(69, 100, true));
    r.run(4800);
    CHECK(src->auditionNote(69, 0, false));
    auto l = r.run(48000);
    CHECK(peak(l, 30000, l.size()) > 0.02f);                      // still sounding: the pedal holds it
    CHECK(src->auditionController(64, 0));
    r.run(24000);
    l = r.run(24000);
    CHECK(peak(l, 0, l.size()) < 1e-5f);                          // released after the pedal came up
    CHECK(!src->auditionController(200, 1));
  }
  {  // pitch bend moves the pitch: a full bend up of a 440 Hz note gives about 494 Hz
    auto freq = [](const std::vector<float>& x, std::size_t a, std::size_t b) {
      int cross = 0; for (std::size_t i = a + 1; i < b; ++i) if (x[i - 1] <= 0.f && x[i] > 0.f) ++cross;
      return cross * static_cast<double>(kRate) / static_cast<double>(b - a); };
    Song song; Rig r(song.s); auto* src = song.s.track(song.t)->source;
    src->auditionNote(69, 100, true);
    auto l = r.run(24000);
    const double f0 = freq(l, 12000, 24000);
    src->auditionPitchBend(16383);
    l = r.run(24000);
    const double f1 = freq(l, 12000, 24000);
    CHECK(f0 > 400 && f0 < 480);
    CHECK(f1 / f0 > 1.10 && f1 / f0 < 1.13);                      // two semitones is 1.122
  }
  {  // mod wheel at full adds vibrato: the pitch is no longer steady between blocks
    Song a, b; Rig ra(a.s), rb(b.s);
    a.s.track(a.t)->source->auditionNote(69, 100, true); b.s.track(b.t)->source->auditionNote(69, 100, true);
    b.s.track(b.t)->source->auditionController(1, 127);
    auto la = ra.run(48000), lb = rb.run(48000);
    double diff = 0; for (std::size_t i = 24000; i < la.size(); ++i) diff += std::fabs(la[i] - lb[i]);
    CHECK(diff > 1.0);
  }
}

void testNoInstrumentNoCrash() {
  Session s(kRate, 2, 256);
  const auto t = s.addTrack("Empty");
  music::MidiClip c; c.notes.push_back({60, 0.0, 1.0, 100});
  s.track(t)->source->setMidiSequence(buildMidiSequence(c, 120.0, kRate));
  Rig r(s);
  r.e->transport().play();
  auto l = r.run(48000);
  CHECK(peak(l, 0, l.size()) == 0.f);
  CHECK(s.track(t)->source->auditionNote(60, 100, true));         // queued, drained, nothing to play it: no growth
  r.run(4800);
}

void testInstrumentRemovedKeepsWorking() {
  Song song(8.0);
  Rig r(song.s);
  r.e->transport().play();
  r.run(36000);
  r.e->transport().stop();
  r.run(4800);
  CHECK(song.s.removeInsert(song.t, 0));                          // targets are refreshed: no dangling pointer
  r.e->transport().play();
  r.run(48000);
  auto syn = std::make_unique<WeztSynth>();
  CHECK(song.s.addInsert(song.t, std::move(syn), WeztSynth::descriptor()));
  r.e->transport().locate(0);
  const auto l = r.run(60000);
  CHECK(peak(l, 30000, 60000) > 0.001f);                          // the new instrument plays the track's part
}

void testParametersAndState() {
  WeztSynth s;
  CHECK(s.parameters().size() == s.parameterCount() && s.parameterCount() == 16);
  const int cut = paramId(s, "Cutoff");
  CHECK(cut >= 0);
  s.setParameterNormalized(static_cast<unsigned>(cut), 0.5);
  CHECK(std::fabs(s.parameterNormalized(static_cast<unsigned>(cut)) - 0.5) < 1e-9);
  CHECK(s.parameterText(static_cast<unsigned>(cut), 0.5).find("Hz") != std::string::npos);  // geometric middle of 40..18000 ~ 848 Hz
  CHECK(!s.setParameterNormalized(999, 0.5));
  CHECK(!s.setParameterNormalized(0, std::nan("")));
  s.setPreset("bass");
  const auto state = s.saveState();
  WeztSynth t;
  CHECK(t.loadState(state) && t.preset() == "bass");
  CHECK(std::fabs(t.parameterNormalized(static_cast<unsigned>(cut)) - s.parameterNormalized(static_cast<unsigned>(cut))) < 1e-6);
  CHECK(!t.loadState({1, 2, 3}));

  Sampler sm;
  CHECK(sm.parameterCount() == sm.parameters().size() && sm.parameterCount() > 10);
  const int sc = paramId(sm, "Cutoff");
  CHECK(sc >= 0 && sm.params().filter == instruments::FilterType::Off);
  CHECK(sm.setParameterNormalized(static_cast<unsigned>(sc), 0.25));
  CHECK(sm.params().filter == instruments::FilterType::LowPass && sm.params().cutoffHz > 20.f && sm.params().cutoffHz < 1000.f);
  CHECK(std::fabs(sm.parameterNormalized(static_cast<unsigned>(sc)) - 0.25) < 1e-4);
  CHECK(sm.parameterText(static_cast<unsigned>(sc), 1.0).find("kHz") != std::string::npos);
  const int g = paramId(sm, "Gain");
  sm.setParameterNormalized(static_cast<unsigned>(g), 1.0);
  CHECK(std::fabs(sm.params().masterGainDb - 12.f) < 1e-4);
}

}  // namespace

int main() {
  testSynthSampleAccurate();
  testSynthPitchAndRelease();
  testSynthDrumsAndVelocity();
  testSamplerSampleAccurate();
  testSequenceBuild();
  testSequencerThroughSession();
  testStopAndLocateReleaseNotes();
  testKeyPreview();
  testKeyboardControllers();
  testNoInstrumentNoCrash();
  testInstrumentRemovedKeepsWorking();
  testParametersAndState();
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("midi tests passed\n");
  return 0;
}
