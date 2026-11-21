// Automation of plugin parameters: lanes follow the transport on the audio thread, survive save/load and
// duplication, never touch a parameter while stopped, and are safe against edits made while playing.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "audio/audio_device.h"
#include "dsp/effect_plugins.h"
#include "engine/engine.h"
#include "instruments/builtin.h"
#include "session/midi_player.h"
#include "session/session.h"

using namespace wezt;
using namespace wezt::session;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("FAIL line %d: %s=%g vs %g\n", __LINE__, #a, _a, _b); ++failures; } } while (0)

namespace {
constexpr double kSr = 48000;
struct Rig {
  Session& s; std::unique_ptr<Engine> e; audio::NullDevice dev;
  explicit Rig(Session& session) : s(session) {
    e = std::make_unique<Engine>(s.graph(), s.maxBlock(), s.channels(), s.sampleRate(), 2);
    s.attach(e->context());
    audio::AudioConfig c; c.sampleRate = static_cast<unsigned>(s.sampleRate()); c.inChannels = 0; c.outChannels = 2;
    std::string err; CHECK(dev.open(c, e.get(), &err) && dev.start(&err));
  }
  std::vector<float> run(unsigned frames, unsigned block = 256) {
    std::vector<float> all;
    for (unsigned done = 0; done < frames; done += block) {
      std::vector<float> l(block), r(block); float* op[2] = {l.data(), r.data()};
      e->process(nullptr, op, block);
      all.insert(all.end(), l.begin(), l.end());
    }
    return all;
  }
};
std::shared_ptr<AudioBuffer> sine(double hz, std::size_t n, float amp) {
  auto b = std::make_shared<AudioBuffer>(1, n);
  for (std::size_t i = 0; i < n; ++i) b->channel(0)[i] = amp * static_cast<float>(std::sin(2 * M_PI * hz * static_cast<double>(i) / kSr));
  return b;
}
double rmsDb(const std::vector<float>& v, double t0, double t1) {
  double s = 0; std::size_t n = 0;
  for (std::size_t i = static_cast<std::size_t>(t0 * kSr); i < static_cast<std::size_t>(t1 * kSr) && i < v.size(); ++i, ++n) s += double(v[i]) * v[i];
  return 10 * std::log10(s / static_cast<double>(std::max<std::size_t>(1, n)) + 1e-20);
}
Clip clipOf(std::shared_ptr<AudioBuffer> b, std::int64_t start) { Clip c; c.audio = std::move(b); c.start = start; c.length = static_cast<std::int64_t>(c.audio->frames); return c; }
struct Made { std::unique_ptr<Session> s; TrackId t; };
Made make(const char* effect = "Wezt EQ") {
  Made m; m.s = std::make_unique<Session>(48000, 2, 128);
  m.t = m.s->addTrack("T", true);
  m.s->addClip(m.t, clipOf(sine(18000, 48000 * 3, 0.1f), 0));
  plugins::PluginDescriptor d; d.name = effect; d.format = plugins::Format::Builtin; d.bitness = 64;
  CHECK(m.s->addInsert(m.t, instruments::makeBuiltin(d), d));
  return m;
}
}  // namespace

int main() {
  const unsigned kHigh = dsp::EqPlugin::HighGain;   // -15..15 dB: 0.5 = 0 dB, 0.9 = +12 dB
  {  // a lane moves a parameter while the transport plays
    auto m = make(); Session& s = *m.s;
    CHECK(s.setParamAutomation(m.t, 0, kHigh, {{0, 0.5f}, {48000, 0.9f}}));
    CHECK(!s.setParamAutomation(m.t, 0, 999, {{0, 0.5f}}));   // no such parameter
    CHECK(!s.setParamAutomation(m.t, 5, kHigh, {{0, 0.5f}}));   // no such insert
    CHECK(!s.setParamAutomation(99, 0, kHigh, {{0, 0.5f}}));
    CHECK(!s.setParamAutomation(m.t, 0, kHigh, {{0, 0.5f}, {10, std::nanf("")}}));   // not a number
    Rig r(s);
    r.e->transport().play();
    const auto out = r.run(static_cast<unsigned>(kSr * 2));
    const double a = rmsDb(out, 0.05, 0.15), mid = rmsDb(out, 0.45, 0.55), z = rmsDb(out, 1.4, 1.9);
    std::printf("EQ high gain automation: start %.1f dB, middle %.1f dB, end %.1f dB\n", a, mid, z);
    NEAR(z - a, 12.0, 1.5);
    CHECK(mid > a + 3 && mid < z - 3);   // rising smoothly in between
    r.e->transport().stop();
    r.run(2048);
    NEAR(s.track(m.t)->inserts[0].instance->parameterNormalized(kHigh), 0.9, 0.01);   // stopped: the last automated value stays
    // the user moves the control while stopped; playing again from the start hands control back to the lane
    s.track(m.t)->inserts[0].instance->setParameterNormalized(kHigh, 0.3);
    r.e->transport().locate(0); r.e->transport().play(); r.run(1024);
    NEAR(s.track(m.t)->inserts[0].instance->parameterNormalized(kHigh), 0.5, 0.01);
  }
  {  // undo and redo of automation edits
    auto m = make(); Session& s = *m.s; edit::UndoStack undo;
    const std::vector<AutomationPoint> a{{0, 0.5f}, {48000, 0.9f}}, b{{0, 0.5f}, {24000, 0.2f}, {48000, 0.9f}};
    CHECK(s.setParamAutomationUndoable(undo, m.t, 0, kHigh, a, "Draw"));
    CHECK(undo.canUndo() && s.paramAutomation(m.t, 0, kHigh) && s.paramAutomation(m.t, 0, kHigh)->points() == a);
    CHECK(s.setParamAutomationUndoable(undo, m.t, 0, kHigh, a, "Draw again"));   // nothing changed: no extra step
    CHECK(undo.history().size() == 1);
    CHECK(s.setParamAutomationUndoable(undo, m.t, 0, kHigh, b, "Edit"));
    CHECK(s.paramAutomation(m.t, 0, kHigh)->points() == b);
    CHECK(undo.undo() && s.paramAutomation(m.t, 0, kHigh)->points() == a);
    CHECK(undo.undo() && s.paramAutomation(m.t, 0, kHigh) == nullptr);   // undoing the first drawing removes the lane
    CHECK(undo.redo() && s.paramAutomation(m.t, 0, kHigh)->points() == a);
    CHECK(undo.redo() && s.paramAutomation(m.t, 0, kHigh)->points() == b);
    // a clear can be undone
    CHECK(s.setParamAutomationUndoable(undo, m.t, 0, kHigh, {}, "Clear") && s.paramAutomation(m.t, 0, kHigh) == nullptr);
    CHECK(undo.undo() && s.paramAutomation(m.t, 0, kHigh)->points() == b);
    // a recording pass: many small steps with one key undo in one go
    edit::UndoStack pass; std::vector<AutomationPoint> grow;
    for (int i = 0; i < 20; ++i) { grow.push_back({i * 1000, 0.5f + 0.02f * i}); CHECK(s.setParamAutomationUndoable(pass, m.t, 0, kHigh, grow, "Record", "rec")); }
    CHECK(pass.history().size() == 1 && s.paramAutomation(m.t, 0, kHigh)->points() == grow);
    CHECK(pass.undo() && s.paramAutomation(m.t, 0, kHigh)->points() == b && !pass.canUndo());   // back to before the pass
    // refused edits leave no step, and a stale step does not crash
    edit::UndoStack u2;
    CHECK(!s.setParamAutomationUndoable(u2, m.t, 0, 999, a, "bad") && !u2.canUndo());
    CHECK(s.setParamAutomationUndoable(u2, m.t, 0, kHigh, a, "x"));
    s.removeTrack(m.t);
    CHECK(u2.undo());   // the track is gone: nothing to restore, and no crash
  }
  {  // effects are served inside the block, in pieces of at most 32 frames, each with the value of its own middle:
     // after the first 128-frame block of a 0 -> 1 ramp over 1 s the last piece (frames 96..128) has set the value at 112
    auto m = make(); Session& s = *m.s;
    CHECK(s.setParamAutomation(m.t, 0, kHigh, {{0, 0.0f}, {48000, 1.0f}}));
    Rig r(s); r.e->transport().play(); r.run(128, 128);
    const double got = s.track(m.t)->inserts[0].instance->parameterNormalized(kHigh);
    std::printf("value after the first 128-frame block of a 1 s ramp: %.5f (middle of the last 32-frame piece: %.5f)\n", got, 112.0 / 48000.0);
    NEAR(got, 112.0 / 48000.0, 0.0002);
  }
  {  // a probe effect records every piece it is asked to process and the value it held then
    struct Probe : plugins::IPluginInstance {
      std::vector<std::pair<std::size_t, double>>* log; std::atomic<double> v{0.0};
      explicit Probe(std::vector<std::pair<std::size_t, double>>* l) : log(l) {}
      std::size_t parameterCount() const override { return 1; }
      std::vector<std::uint8_t> saveState() const override { return {}; }
      bool loadState(const std::vector<std::uint8_t>&) override { return true; }
      std::vector<plugins::ParamInfo> parameters() const override { plugins::ParamInfo p; p.id = 0; p.name = "x"; return {p}; }
      double parameterNormalized(unsigned) const override { return v.load(); }
      bool automateParameter(unsigned, double n) override { v.store(n); return true; }
      bool supportsAutomation() const override { return true; }
      std::atomic<bool> latFlag{false}; std::atomic<std::size_t> lat{0};
      std::size_t latencySamples() const override { return lat.load(); }
      bool takeLatencyChanged() override { return latFlag.exchange(false); }   // reported once, like the real hosts
      void process(const AudioBuffer& in, AudioBuffer& out) override {
        log->push_back({out.frames, v.load()});
        for (std::size_t c = 0; c < out.channels; ++c) for (std::size_t i = 0; i < out.frames; ++i) out.channel(c)[i] = in.channel(std::min(c, in.channels - 1))[i];
      }
    };
    std::vector<std::pair<std::size_t, double>> log;
    Session s(48000, 2, 256); const TrackId t = s.addTrack("P", true);
    s.addClip(t, clipOf(sine(440, 48000, 0.1f), 0));
    plugins::PluginDescriptor d; d.name = "Probe"; d.format = plugins::Format::Builtin;
    CHECK(s.addInsert(t, std::make_unique<Probe>(&log), d));
    {  // a plug-in announcing a new latency: the session notices and re-computes the compensation
      auto* probe = static_cast<Probe*>(s.track(t)->inserts[0].instance);
      CHECK(!s.pollPluginLatency() && !s.applyPluginLatency());
      probe->lat.store(50); probe->latFlag.store(true);
      CHECK(s.pollPluginLatency());                 // collected, still waiting for the audio to be stopped
      CHECK(s.pollPluginLatency());
      CHECK(s.applyPluginLatency());
      CHECK(s.graph().outputLatency() >= 50);
      CHECK(!s.pollPluginLatency() && !s.applyPluginLatency());   // applied once
    }
    // a jump between frames 100 and 101 inside the first 256-frame block, then a steady value
    CHECK(s.setParamAutomation(t, 0, 0, {{0, 0.2f}, {100, 0.2f}, {101, 0.8f}, {1000000, 0.8f}}));
    Rig r(s); r.e->transport().play();
    auto out = r.run(256, 256);
    std::size_t total = 0; for (auto& e : log) total += e.first;
    CHECK(total == 256);                                   // every frame processed exactly once
    double before = -1, after = -1; std::size_t pos = 0;
    for (auto& e : log) { if (pos + e.first <= 100) before = e.second; else if (pos >= 101 && after < 0) after = e.second; pos += e.first; }
    std::printf("probe pieces: %zu, before the jump %.3f, after it %.3f\n", log.size(), before, after);
    CHECK(log.size() >= 2);
    NEAR(before, 0.2, 0.01); NEAR(after, 0.8, 0.01);       // the change lands at frame 100, not at the block edge
    bool boundaryAt100 = false; pos = 0; for (auto& e : log) { pos += e.first; if (pos == 100) boundaryAt100 = true; }
    CHECK(boundaryAt100);
    // audio passes through intact across the cuts
    double lo = 1e9, hi = -1e9;   // out = ref * (the track's constant pan gain): the ratio must not wobble at the cuts
    for (std::size_t i = 0; i < 256; ++i) { const double ref = 0.1 * std::sin(2 * M_PI * 440 * double(i) / kSr); if (std::fabs(ref) > 0.02) { const double q = out[i] / ref; lo = std::min(lo, q); hi = std::max(hi, q); } }
    CHECK(hi - lo < 1e-3);
  }
  {  // an instrument is run in pieces too, and a note still starts on its own frame: the track's MIDI is cut up with the block
    struct Synth : plugins::IPluginInstance {
      struct Piece { std::size_t start, frames; std::vector<std::pair<std::size_t, int>> notes; double v; };
      std::vector<Piece>* log; std::atomic<double> v{0.0}; std::vector<MidiEvent> pending; std::size_t pos = 0;
      explicit Synth(std::vector<Piece>* l) : log(l) {}
      std::size_t parameterCount() const override { return 1; }
      std::vector<std::uint8_t> saveState() const override { return {}; }
      bool loadState(const std::vector<std::uint8_t>&) override { return true; }
      std::vector<plugins::ParamInfo> parameters() const override { plugins::ParamInfo p; p.id = 0; p.name = "x"; return {p}; }
      double parameterNormalized(unsigned) const override { return v.load(); }
      bool automateParameter(unsigned, double n) override { v.store(n); return true; }
      bool supportsAutomation() const override { return true; }
      bool acceptsMidi() const override { return true; }
      void handleMidi(const MidiEvent* e, std::size_t n) override { pending.assign(e, e + n); }
      void process(const AudioBuffer&, AudioBuffer& out) override {
        Piece p{pos, out.frames, {}, v.load()};
        for (const auto& e : pending) if (e.type == MidiEvent::NoteOn) p.notes.push_back({e.offset, e.a});
        pending.clear();
        log->push_back(p); pos += out.frames;
        for (std::size_t c = 0; c < out.channels; ++c) for (std::size_t i = 0; i < out.frames; ++i) out.channel(c)[i] = 0.f;
      }
    };
    std::vector<Synth::Piece> log;
    Session s(48000, 2, 256); const TrackId t = s.addTrack("Syn", false);
    plugins::PluginDescriptor d; d.name = "Synth"; d.format = plugins::Format::Builtin;
    CHECK(s.addInsert(t, std::make_unique<Synth>(&log), d));
    music::MidiClip c; c.notes.push_back({60, 100.0 / 24000.0, 0.5, 100});   // 120 bpm: one beat = 24000 frames, so this is frame 100
    c.notes.push_back({64, 200.0 / 24000.0, 0.5, 100});
    s.track(t)->source->setMidiSequence(session::buildMidiSequence(c, 120.0, 48000));
    CHECK(s.setParamAutomation(t, 0, 0, {{0, 0.0f}, {48000, 1.0f}}));
    Rig r(s); r.e->transport().play(); r.run(256, 256);
    std::size_t total = 0, n60 = 0, n64 = 0; long at60 = -1, at64 = -1; bool varied = false; double first = log.empty() ? 0 : log[0].v;
    for (auto& p : log) {
      total += p.frames;
      for (auto& nt : p.notes) { if (nt.second == 60) { ++n60; at60 = static_cast<long>(p.start + nt.first); } if (nt.second == 64) { ++n64; at64 = static_cast<long>(p.start + nt.first); } }
      if (std::fabs(p.v - first) > 1e-4) varied = true;
    }
    std::printf("synth pieces: %zu, note 60 at %ld, note 64 at %ld\n", log.size(), at60, at64);
    CHECK(total == 256 && log.size() >= 4);                 // cut into pieces, every frame once
    CHECK(n60 == 1 && n64 == 1);                            // every note delivered exactly once
    CHECK(std::labs(at60 - 100) <= 1 && std::labs(at64 - 200) <= 1);   // on its own frame
    CHECK(varied);                                          // the parameter moved between pieces
  }
  {  // save, load, duplicate
    auto m = make(); Session& s = *m.s;
    CHECK(s.setParamAutomation(m.t, 0, kHigh, {{0, 0.5f}, {24000, 0.75f}, {48000, 0.9f}}));
    const std::string f = "param_auto_test.wsession";
    std::string err; CHECK(s.save(f, &err));
    auto loaded = Session::load(f, [](const plugins::PluginDescriptor& d) { return instruments::makeBuiltin(d); }, &err);
    CHECK(loaded != nullptr);
    if (loaded) {
      const AutomationLane* l = loaded->paramAutomation(0, 0, kHigh);
      CHECK(l && l->points().size() == 3); if (l) { NEAR(l->points()[1].value, 0.75, 1e-4); CHECK(l->points()[1].frame == 24000); }
      Rig r(*loaded); r.e->transport().play();
      r.run(static_cast<unsigned>(kSr * 1.5));   // (the in-memory test clip is not saved, so check the parameter itself)
      NEAR(loaded->track(0)->inserts[0].instance->parameterNormalized(kHigh), 0.9, 0.01);   // the loaded session automates too
    }
    std::remove(f.c_str());
    const TrackId d = s.duplicateTrack(m.t, [](const plugins::PluginDescriptor& dd) { return instruments::makeBuiltin(dd); });
    CHECK(d >= 0); const AutomationLane* dl = d >= 0 ? s.paramAutomation(d, 0, kHigh) : nullptr; CHECK(dl && dl->points().size() == 3);
    // an empty point list removes the lane
    CHECK(s.setParamAutomation(m.t, 0, kHigh, {})); CHECK(s.paramAutomation(m.t, 0, kHigh) == nullptr);
  }
  {  // removing the plugin while a lane exists must not leave the audio thread holding a dead pointer
    auto m = make(); Session& s = *m.s;
    CHECK(s.setParamAutomation(m.t, 0, kHigh, {{0, 0.5f}, {48000, 0.9f}}));
    Rig r(s); r.e->transport().play(); r.run(4096);
    CHECK(s.removeInsert(m.t, 0)); r.run(4096);
    plugins::PluginDescriptor d = dsp::CompressorPlugin::descriptor();
    CHECK(s.addInsert(m.t, instruments::makeBuiltin(d), d)); r.run(4096);
  }
  {  // other built-ins: compressor makeup gain and the synth's filter, delivered through the same call
    auto m = make("Wezt Compressor"); Session& s = *m.s;
    // threshold at 0 dB so nothing is compressed; makeup 0 -> 12 dB over two seconds (0 .. 24 dB range: 0.5 = 12 dB)
    s.track(m.t)->inserts[0].instance->setParameterNormalized(dsp::CompressorPlugin::Threshold, 1.0);
    CHECK(s.setParamAutomation(m.t, 0, dsp::CompressorPlugin::Makeup, {{0, 0.f}, {96000, 0.5f}}));
    Rig r(s); r.e->transport().play();
    const auto out = r.run(static_cast<unsigned>(kSr * 2.2));
    const double a = rmsDb(out, 0.05, 0.15), z = rmsDb(out, 2.0, 2.15);
    std::printf("compressor makeup automation: %.1f dB -> %.1f dB\n", a, z);
    NEAR(z - a, 12.0, 1.5);
  }
  {  // edits from another thread while the lanes play: nothing crashes, nothing is lost (run under ASan/TSan)
    auto m = make(); Session& s = *m.s;
    CHECK(s.setParamAutomation(m.t, 0, kHigh, {{0, 0.5f}, {96000, 0.9f}}));
    Rig r(s); r.e->transport().play();
    std::atomic<bool> stop{false};
    std::thread control([&] {
      int k = 0;
      while (!stop.load()) {
        s.track(m.t)->inserts[0].instance->setParameterNormalized(dsp::EqPlugin::LowGain, (k++ % 100) / 100.0);   // another parameter, from a control thread
        s.setParamAutomation(m.t, 0, kHigh, {{0, 0.5f}, {static_cast<std::int64_t>(48000 + (k % 5) * 1000), 0.9f}});    // republished while playing
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
    for (int i = 0; i < 400; ++i) r.run(512);
    stop = true; control.join();
    CHECK(s.paramAutomation(m.t, 0, kHigh) != nullptr);
  }
  {  // the Sampler: lanes move its parameters, and a control thread setting another one at the same time loses neither
    auto m = make("Wezt Sampler"); Session& s = *m.s;
    auto* smp = s.track(m.t)->inserts[0].instance;
    CHECK(smp->supportsAutomation());
    CHECK(s.setParamAutomation(m.t, 0, 9, {{0, 0.f}, {24000, 1.f}}));    // master gain: whole range over half a second
    Rig r(s); r.e->transport().play();
    std::atomic<bool> stop{false};
    std::thread control([&] { int k = 0; while (!stop.load()) { smp->setParameterNormalized(10, 0.25 + 0.5 * ((k++ % 2) ? 1 : 0)); std::this_thread::yield(); } });   // pan, from a control thread
    for (int i = 0; i < 200; ++i) r.run(256);
    stop = true; control.join();
    smp->setParameterNormalized(10, 0.7);
    r.run(1024);
    NEAR(smp->parameterNormalized(9), 1.0, 0.01);     // the automated parameter reached the lane's end
    NEAR(smp->parameterNormalized(10), 0.7, 0.01);    // and the manual change was not overwritten
    // filter cutoff turns the filter on, as it does when set by hand
    CHECK(s.setParamAutomation(m.t, 0, 1, {{0, 0.3f}}));
    r.e->transport().stop(); r.e->transport().locate(0); r.e->transport().play(); r.run(1024);
    NEAR(smp->parameterNormalized(1), 0.3, 0.01);
  }
  if (failures == 0) std::printf("parameter automation tests passed\n");
  return failures == 0 ? 0 : 1;
}
