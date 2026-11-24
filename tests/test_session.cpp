// Session layer: playback timing, mixer maths, routing, delay compensation, automation,
// recording to disk, save/load (including damaged files) and offline bounce.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>

#include "audio/audio_device.h"
#include "engine/delay_line.h"
#include "io/wav.h"
#include "edit/undo.h"
#include "session/session.h"

using namespace wezt;
using namespace wezt::session;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("FAIL %s:%d %s=%g vs %g\n", __FILE__, __LINE__, #a, _a, _b); ++failures; } } while (0)

namespace {

struct Out { std::vector<float> l, r; };

// Drives the session through a real Engine with callbacks of the given sizes.
struct Rig {
  Session& s;
  std::unique_ptr<Engine> e;
  audio::NullDevice dev;
  Rig(Session& session, unsigned devIn = 0, unsigned inLat = 0, unsigned outLat = 0) : s(session) {
    e = std::make_unique<Engine>(s.graph(), s.maxBlock(), s.channels(), s.sampleRate(), 2);
    s.attach(e->context());
    audio::AudioConfig c;
    c.sampleRate = static_cast<unsigned>(s.sampleRate());
    c.inChannels = devIn; c.outChannels = 2;
    c.inputLatencyFrames = inLat; c.outputLatencyFrames = outLat;
    std::string err;
    CHECK(dev.open(c, e.get(), &err) && dev.start(&err));
    devIn_ = devIn;
  }
  // `input(k, ch)` gives the device input sample for the k-th frame since the first call.
  template <class F>
  Out run(const std::vector<unsigned>& sizes, F input) {
    Out o;
    for (unsigned n : sizes) {
      std::vector<std::vector<float>> in(devIn_, std::vector<float>(n)), out(2, std::vector<float>(n));
      std::vector<const float*> ip;
      std::vector<float*> op;
      for (unsigned c = 0; c < devIn_; ++c) { for (unsigned i = 0; i < n; ++i) in[c][i] = input(fed_ + i, c); ip.push_back(in[c].data()); }
      for (auto& v : out) op.push_back(v.data());
      e->process(devIn_ ? ip.data() : nullptr, op.data(), n);
      fed_ += n;
      o.l.insert(o.l.end(), out[0].begin(), out[0].end());
      o.r.insert(o.r.end(), out[1].begin(), out[1].end());
    }
    return o;
  }
  Out run(const std::vector<unsigned>& sizes) { return run(sizes, [](std::int64_t, unsigned) { return 0.f; }); }
  unsigned devIn_ = 0;
  std::int64_t fed_ = 0;
};

std::vector<unsigned> sizesFor(unsigned total) {  // awkward callback sizes that add up to `total`
  std::vector<unsigned> v;
  const unsigned pat[] = {1, 7, 128, 300, 3, 61, 200};
  unsigned i = 0;
  while (total) { const unsigned n = std::min(total, pat[i++ % 7]); v.push_back(n); total -= n; }
  return v;
}

Clip clipOf(std::shared_ptr<AudioBuffer> b, std::int64_t start) {
  Clip c;
  c.audio = b;
  c.start = start;
  c.length = static_cast<std::int64_t>(b->frames);
  return c;
}
std::shared_ptr<AudioBuffer> mono(std::size_t n, float v) {
  auto b = std::make_shared<AudioBuffer>(1, n);
  std::fill(b->channel(0), b->channel(0) + n, v);
  return b;
}
std::shared_ptr<AudioBuffer> impulses(std::size_t n, std::initializer_list<std::pair<std::size_t, float>> at) {
  auto b = std::make_shared<AudioBuffer>(1, n);
  for (auto& p : at) b->channel(0)[p.first] = p.second;
  return b;
}

// A stand-in plugin: gain with a reported latency and a 4-byte state.
class FakePlugin : public plugins::IPluginInstance {
 public:
  FakePlugin(float g, std::size_t latency) : g_(g), n_(latency) {}
  void prepare(double, std::size_t) override { d_.setDelay(n_, 2); }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    for (std::size_t c = 0; c < out.channels; ++c)
      for (std::size_t i = 0; i < out.frames; ++i) out.channel(c)[i] = in.channel(c)[i] * g_;
    d_.process(out);
  }
  std::size_t latencySamples() const override { return n_; }
  std::size_t parameterCount() const override { return 1; }
  std::vector<std::uint8_t> saveState() const override { std::vector<std::uint8_t> v(sizeof g_); std::memcpy(v.data(), &g_, sizeof g_); return v; }
  bool loadState(const std::vector<std::uint8_t>& s) override { if (s.size() != sizeof g_) return false; std::memcpy(&g_, s.data(), sizeof g_); return true; }
  float gain() const { return g_; }
 private:
  float g_;
  std::size_t n_;
  DelayLine d_;
};
plugins::PluginDescriptor fakeDesc() { plugins::PluginDescriptor d; d.name = "Fake"; d.vendor = "Test"; d.path = "/nowhere/fake.vst3"; d.bitness = 64; return d; }

fs::path tmpDir(const char* name) {
  const fs::path d = fs::temp_directory_path() / (std::string("wezt_session_") + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

int firstNonZero(const std::vector<float>& v, float eps = 1e-9f) {
  for (std::size_t i = 0; i < v.size(); ++i) if (std::fabs(v[i]) > eps) return static_cast<int>(i);
  return -1;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
static void testPlaybackAndPan() {
  Session s(48000, 2, 128);
  const auto t = s.addTrack("Mono", true);
  s.addClip(t, clipOf(impulses(500, {{0, 1.f}, {100, 0.5f}}), 1000));
  Rig r(s);
  r.e->transport().play();
  const Out o = r.run(sizesFor(2000));
  CHECK(o.l.size() == 2000);
  CHECK(firstNonZero(o.l) == 1000);  // exactly at the clip start, whatever the callback sizes
  NEAR(o.l[1000], std::cos(M_PI / 4), 1e-6);   // constant-power pan, -3 dB at centre
  NEAR(o.r[1000], std::sin(M_PI / 4), 1e-6);
  NEAR(o.l[1100], 0.5 * std::cos(M_PI / 4), 1e-6);
  NEAR(o.l[1101], 0.0, 1e-9);

  // Hard left / right.
  s.track(t)->strip->setPan(-1.f);
  r.e->transport().locate(1000);
  Out o2 = r.run({128, 128});  // let the ramp settle
  r.e->transport().locate(1000);
  o2 = r.run({128});
  NEAR(o2.l[0], 1.0, 1e-5);
  NEAR(o2.r[0], 0.0, 1e-5);

  // Stereo track uses balance: unity at centre, far side turned down.
  const auto st = s.addTrack("Stereo", false);
  s.track(t)->strip->setMute(true);
  auto sb = std::make_shared<AudioBuffer>(2, 300);
  std::fill(sb->channel(0), sb->channel(0) + 300, 0.8f);
  std::fill(sb->channel(1), sb->channel(1) + 300, 0.4f);
  s.addClip(st, clipOf(sb, 0));
  s.track(st)->strip->setPan(0.5f);
  Rig r2(s);
  r2.e->transport().play();
  const Out o3 = r2.run({128});
  NEAR(o3.l[5], 0.8 * 0.5, 1e-5);
  NEAR(o3.r[5], 0.4, 1e-5);
}

static void testMuteSolo() {
  Session s(48000, 2, 128);
  const auto a = s.addTrack("A", true), b = s.addTrack("B", true);
  s.addClip(a, clipOf(mono(5000, 0.25f), 0));
  s.addClip(b, clipOf(mono(5000, 0.5f), 0));
  s.track(a)->strip->setPan(-1.f);
  s.track(b)->strip->setPan(-1.f);
  Rig r(s);
  r.e->transport().play();
  Out o = r.run({128});
  NEAR(o.l[100], 0.75, 1e-5);

  s.setSolo(a, true);  // B must go silent, click-free
  o = r.run({128, 128});
  bool smooth = true;
  for (std::size_t i = 1; i < 128; ++i) smooth = smooth && std::fabs(o.l[i] - o.l[i - 1]) < 0.01f;
  CHECK(smooth);
  NEAR(o.l[255], 0.25, 1e-5);
  CHECK(s.anySolo());
  s.setSolo(a, false);
  o = r.run({256});
  NEAR(o.l[255], 0.75, 1e-5);

  s.track(a)->strip->setMute(true);
  o = r.run({256});
  NEAR(o.l[255], 0.5, 1e-5);
  s.track(a)->strip->setMute(false);
  s.track(a)->strip->setGainDb(-6.0206f);
  o = r.run({256});
  NEAR(o.l[255], 0.125 + 0.5, 1e-4);
}

static void testSends() {
  Session s(48000, 2, 128);
  const auto t = s.addTrack("T", true);
  const auto fx = s.addBus("FX");
  s.addClip(t, clipOf(mono(10000, 1.f), 0));
  s.track(t)->strip->setPan(-1.f);  // left only: L gain 1
  const int post = s.addSend(t, fx, -6.f, false);
  CHECK(post == 0);
  Rig r(s);
  r.e->transport().play();
  Out o = r.run({128, 128});
  NEAR(o.l[255], 1.0 + dbToLin(-6.f), 1e-4);  // direct + post-fader send

  s.setSendMute(t, 0, true);
  o = r.run({256});
  NEAR(o.l[255], 1.0, 1e-5);
  s.setSendMute(t, 0, false);

  // Pre-fader send keeps feeding while the fader is down; post-fader does not.
  s.track(t)->strip->setGainDb(-100.f);
  o = r.run({256});
  NEAR(o.l[255], 0.0, 1e-6);  // post-fader send follows the fader
  s.removeSend(t, 0);
  CHECK(s.addSend(t, fx, -6.f, true) == 0);
  o = r.run({256});
  NEAR(o.l[255], dbToLin(-6.f), 1e-4);  // pre-fader: dual-mono source, unity bus (balance)
  NEAR(o.r[255], dbToLin(-6.f), 1e-4);

  // Routing loops are refused and leave a working graph.
  const auto a = s.addBus("A"), b = s.addBus("B");
  CHECK(s.setBusOutput(a, b));
  CHECK(!s.setBusOutput(b, a));
  CHECK(!s.setBusOutput(a, a));
  CHECK(!s.setBusOutput(kMaster, a));
  CHECK(s.setTrackOutput(t, a));
  o = r.run({128});
  CHECK(std::isfinite(o.l[100]));
  CHECK(s.removeBus(b));  // a falls back to the master
  o = r.run({256});
  CHECK(std::isfinite(o.l[255]));
}

static void testLatencyAlignment() {
  Session s(48000, 2, 128);
  const auto a = s.addTrack("A", true), b = s.addTrack("B", true);
  s.addClip(a, clipOf(impulses(100, {{0, 1.f}}), 500));
  s.addClip(b, clipOf(impulses(100, {{0, 1.f}}), 500));
  s.track(a)->strip->setPan(-1.f);
  s.track(b)->strip->setPan(-1.f);
  CHECK(s.addInsert(a, std::make_unique<FakePlugin>(1.f, 64), fakeDesc()));
  Rig r(s);
  r.e->transport().play();
  const Out o = r.run(sizesFor(1000));
  // Both tracks show up together, 64 samples later: the shorter path was delayed to match.
  CHECK(firstNonZero(o.l) == 564);
  NEAR(o.l[564], 2.0, 1e-5);
  int count = 0;
  for (float v : o.l) if (std::fabs(v) > 1e-6f) ++count;
  CHECK(count == 1);

  // Inserts can be removed and reordered at any position.
  CHECK(s.addInsert(a, std::make_unique<FakePlugin>(0.5f, 0), fakeDesc(), 0));
  CHECK(s.track(a)->inserts.size() == 2);
  CHECK(s.removeInsert(a, 1));
  CHECK(!s.removeInsert(a, 5));
  CHECK(s.track(a)->inserts.size() == 1);
}

static void testAutomation() {
  Session s(48000, 2, 128);
  const auto t = s.addTrack("T", true);
  s.addClip(t, clipOf(mono(4000, 1.f), 0));
  Strip& st = *s.track(t)->strip;
  st.setPan(-1.f);
  st.volumeLane().add(0, 0.f);
  st.volumeLane().add(1024, -20.f);
  st.volumeLane().add(2048, -20.f);
  st.volumeLane().add(3000, -90.f);
  NEAR(st.volumeLane().valueAt(512, 0), -10.0, 1e-5);
  Rig r(s);
  r.e->transport().play();
  const Out o = r.run(std::vector<unsigned>(16, 128));
  NEAR(o.l[1023], dbToLin(-20.f), 1e-5);   // end of a block equals the lane value at that frame
  NEAR(o.l[1535], dbToLin(-20.f), 1e-5);
  CHECK(o.l[10] > o.l[500] && o.l[500] > o.l[1000]);  // falling in between

  st.setAutomationOn(false);  // fader takes over again
  r.e->transport().locate(0);
  const Out o2 = r.run({128, 128});
  NEAR(o2.l[255], 1.0, 1e-5);

  // Mute automation is stepped.
  st.setAutomationOn(true);
  st.volumeLane().clear();
  st.muteLane().add(0, 0.f);
  st.muteLane().add(1000, 1.f);
  r.e->transport().locate(0);
  const Out o3 = r.run(std::vector<unsigned>(10, 128));
  NEAR(o3.l[100], 1.0, 1e-5);
  NEAR(o3.l[1279], 0.0, 1e-5);
}

static void testFadesAndOverlap() {
  Session s(48000, 2, 128);
  const auto t = s.addTrack("T", true);
  s.track(t)->strip->setPan(-1.f);
  Clip a = clipOf(mono(1000, 1.f), 0);
  a.fadeIn = 100; a.fadeOut = 100;
  Clip b = clipOf(mono(1000, 0.5f), 400);  // overlaps the end of a: later clip wins
  b.fadeIn = 200;                          // ... through a cross-fade
  s.addClip(t, a);
  s.addClip(t, b);
  Rig r(s);
  r.e->transport().play();
  const Out o = r.run({128, 128, 128, 128, 128, 128, 128, 128, 128, 128});
  NEAR(o.l[0], 0.0, 1e-6);
  NEAR(o.l[50], 0.5, 1e-5);
  NEAR(o.l[200], 1.0, 1e-5);
  NEAR(o.l[400], 1.0, 1e-5);         // start of the cross-fade: all of a
  NEAR(o.l[500], 0.75, 1e-5);        // halfway: mix of 1.0 and 0.5
  NEAR(o.l[700], 0.5, 1e-5);         // b alone
  NEAR(o.l[1000], 0.5, 1e-5);         // a has ended, b carries on
}

static void testRecording() {
  const fs::path dir = tmpDir("rec");
  auto input = [](std::int64_t k, unsigned c) { return 0.5f * std::sin(0.01f * static_cast<float>(k) + static_cast<float>(c)); };
  {
    Session s(48000, 2, 128);
    s.setRecordFolder((dir / "Audio Files").string());
    const auto t = s.addTrack("Vox", true, {1});
    s.track(t)->strip->setPan(-1.f);
    s.track(t)->source->arm(true);
    Rig r(s, 2, 32, 16);  // 48 frames of round-trip latency
    r.e->transport().locate(2000);
    r.e->transport().record();
    const Out o = r.run(std::vector<unsigned>(5, 128), input);
    // Armed track in Auto mode while recording: the input is audible.
    NEAR(o.l[200], input(200, 1), 1e-5);
    r.e->transport().stop();
    r.run({128}, input);
    std::string err;
    CHECK(s.commitTakes(&err) == 1);
    CHECK(err.empty());
    CHECK(s.track(t)->source->droppedBlocks() == 0);
    const auto list = s.track(t)->source->playlist();
    CHECK(list.size() == 1);
    if (list.size() == 1) {
      CHECK(list[0].start == 2000 - 48);  // placed where the sound happened, not where it arrived
      CHECK(list[0].length == 640);
      CHECK(list[0].audio && list[0].audio->channels == 1);
      double maxErr = 0;
      for (std::int64_t i = 0; i < 640; ++i) maxErr = std::max(maxErr, std::fabs(static_cast<double>(list[0].audio->channel(0)[i]) - input(i, 1)));
      CHECK(maxErr < 2e-7);  // 24-bit
      io::WavReader rd;
      CHECK(rd.open(list[0].file, &err));
      CHECK(rd.info().hasBext && rd.info().timeReference == 1952);
      CHECK(fs::path(list[0].file).parent_path() == dir / "Audio Files");
    }
    // A second take never overwrites the first.
    r.e->transport().locate(0);
    r.e->transport().record();
    r.run({128}, input);
    r.e->transport().stop();
    r.run({128}, input);
    CHECK(s.commitTakes(&err) == 1);
    CHECK(s.track(t)->source->playlist().size() == 2);
    int files = 0;
    for (auto& e : fs::directory_iterator(dir / "Audio Files")) { (void)e; ++files; }
    CHECK(files == 2);
    // Take starting before time zero: the head is trimmed, nothing is lost or shifted.
    r.e->transport().locate(10);
    r.e->transport().record();
    const std::int64_t base = r.fed_;
    r.run({128}, input);
    r.e->transport().stop();
    r.run({128}, input);
    CHECK(s.commitTakes(&err) == 1);
    const auto l3 = s.track(t)->source->playlist();
    if (l3.size() == 3) {
      CHECK(l3[2].start == 0 && l3[2].length == 128 - 38);
      NEAR(l3[2].audio->channel(0)[0], input(base + 38, 1), 2e-7);
    }
    // Unarmed tracks record nothing.
    s.track(t)->source->arm(false);
    r.e->transport().record();
    r.run({128}, input);
    r.e->transport().stop();
    r.run({128}, input);
    CHECK(s.commitTakes(&err) == 0);
  }
}

static void testRecordingErrors() {
  const fs::path dir = tmpDir("recerr");
  // The record folder is a file: nothing can be created; the engine must keep running.
  { std::ofstream f(dir / "blocked"); f << "x"; }
  Session s(48000, 2, 128);
  s.setRecordFolder((dir / "blocked" / "sub").string());
  const auto t = s.addTrack("Vox", true);
  s.track(t)->source->arm(true);
  Rig r(s, 2);
  r.e->transport().record();
  r.run(std::vector<unsigned>(3, 128), [](std::int64_t, unsigned) { return 0.1f; });
  r.e->transport().stop();
  r.run({128});
  std::string err;
  CHECK(s.commitTakes(&err) == 0);
  CHECK(!s.track(t)->source->lastError().empty());
}

static void testSaveLoad() {
  const fs::path dir = tmpDir("save");
  const std::string file = (dir / "My Song.wezt").string();
  fs::create_directories(dir / "Audio Files");
  {
    AudioBuffer b(1, 3000);
    for (std::size_t i = 0; i < 3000; ++i) b.channel(0)[i] = 0.5f * std::sin(0.02f * static_cast<float>(i));
    CHECK(io::writeWavFloat((dir / "Audio Files" / "gtr.wav").string(), b, 48000));
  }
  std::string state;
  {
    Session s(48000, 2, 256);
    const auto fx = s.addBus("Room = verb 100%");
    s.bus(fx)->strip->setGainDb(-3.f);
    s.bus(kMaster)->strip->setGainDb(-1.5f);
    const auto t = s.addTrack("Lead Gtr = 2 %", true, {0, 1});
    const auto t2 = s.addTrack("Keys", false);
    Track& tr = *s.track(t);
    tr.strip->setGainDb(-4.5f);
    tr.strip->setPan(0.25f);
    tr.strip->volumeLane().add(0, 0.f);
    tr.strip->volumeLane().add(1000, -12.f);
    tr.strip->muteLane().add(0, 0.f);
    tr.strip->muteLane().add(500, 1.f);
    s.setSolo(t, true);
    tr.source->setMonitorMode(MonitorMode::Input);
    CHECK(s.addSend(t, fx, -9.f, true) == 0);
    s.setSendMute(t, 0, true);
    CHECK(s.setTrackOutput(t2, fx));
    auto plugin = std::make_unique<FakePlugin>(0.25f, 0);
    { auto d = fakeDesc(); d.sandbox = true; CHECK(s.addInsert(t, std::move(plugin), d)); }   // safe-mode flag must survive a save
    s.setTempo(93.5); s.setBeatsPerBar(7);
    std::string err;
    const int id = s.addClipFromFile(t, (dir / "Audio Files" / "gtr.wav").string(), 480, &err);
    CHECK(id > 0);
    auto list = tr.source->playlist();
    list[0].offset = 100; list[0].length = 2000; list[0].gainDb = -2.f; list[0].fadeIn = 10; list[0].fadeOut = 20;
    tr.source->setPlaylist(list);
    CHECK(s.save(file, &err));
    CHECK(!fs::exists(file + ".tmp"));
    CHECK(s.save(file, &err));  // second save keeps the previous one as .bak
    CHECK(fs::exists(file + ".bak"));
  }
  // The clip is stored relative to the session, so the folder can be moved.
  {
    std::ifstream f(file);
    std::stringstream ss; ss << f.rdbuf();
    state = ss.str();
    CHECK(state.find("file=Audio%20Files/gtr.wav") != std::string::npos);
    CHECK(state.rfind("WEZT-SESSION 1", 0) == 0);
  }
  auto factory = [](const plugins::PluginDescriptor& d) -> std::unique_ptr<plugins::IPluginInstance> {
    if (d.name == "Fake") return std::make_unique<FakePlugin>(1.f, 0);
    return nullptr;
  };
  std::string err;
  auto loaded = Session::load(file, factory, &err);
  CHECK(loaded != nullptr);
  if (loaded) {
    CHECK(loaded->warnings().empty());
    CHECK(std::fabs(loaded->tempo() - 93.5) < 1e-6 && loaded->beatsPerBar() == 7);   // tempo and time signature come back
    CHECK(loaded->sampleRate() == 48000 && loaded->channels() == 2 && loaded->maxBlock() == 256);
    CHECK(loaded->trackCount() == 2);
    const auto ids = loaded->trackIds();
    Track& t = *loaded->track(ids[0]);
    CHECK(!t.inserts.empty() && t.inserts[0].descriptor.sandbox);
    CHECK(t.name == "Lead Gtr = 2 %" && t.mono && t.solo);
    NEAR(t.strip->gainDb(), -4.5, 1e-6);
    NEAR(t.strip->pan(), 0.25, 1e-6);
    CHECK(t.strip->volumeLane().points().size() == 2 && t.strip->muteLane().points().size() == 2);
    NEAR(t.strip->volumeLane().points()[1].value, -12.0, 1e-6);
    CHECK(t.source->inputs().size() == 2 && t.source->inputs()[1] == 1);
    CHECK(t.source->monitorMode() == MonitorMode::Input);
    CHECK(t.sends.size() == 1 && t.sends[0].preFader && t.sends[0].muted);
    NEAR(t.sends[0].levelDb, -9.0, 1e-6);
    CHECK(loaded->bus(t.sends[0].bus)->name == "Room = verb 100%");
    NEAR(loaded->bus(t.sends[0].bus)->strip->gainDb(), -3.0, 1e-6);
    NEAR(loaded->bus(kMaster)->strip->gainDb(), -1.5, 1e-6);
    CHECK(loaded->track(ids[1])->output == t.sends[0].bus);
    CHECK(t.inserts.size() == 1 && t.inserts[0].instance);
    if (t.inserts[0].instance) NEAR(static_cast<FakePlugin*>(t.inserts[0].instance)->gain(), 0.25, 1e-9);  // state came back
    const auto clips = t.source->playlist();
    CHECK(clips.size() == 1);
    if (clips.size() == 1) {
      CHECK(clips[0].start == 480 && clips[0].offset == 100 && clips[0].length == 2000 && clips[0].fadeIn == 10 && clips[0].fadeOut == 20);
      NEAR(clips[0].gainDb, -2.0, 1e-6);
      CHECK(clips[0].audio && clips[0].audio->frames == 3000);
    }
    // The loaded session renders: first audible sample is the clip's (after its fade-in).
    Rig r(*loaded);
    r.e->transport().play();
    const Out o = r.run({512});
    CHECK(firstNonZero(o.l) >= 480);
  }

  // Missing plugin and missing audio do not stop the session from opening; nothing is lost.
  fs::rename(dir / "Audio Files" / "gtr.wav", dir / "Audio Files" / "gtr_moved.wav");
  auto degraded = Session::load(file, nullptr, &err);
  CHECK(degraded != nullptr);
  if (degraded) {
    CHECK(degraded->warnings().size() == 2);
    Track& t = *degraded->track(degraded->trackIds()[0]);
    CHECK(t.inserts.size() == 1 && t.inserts[0].instance == nullptr && t.inserts[0].offlineState.size() == 4);
    CHECK(t.source->playlist().size() == 1 && t.source->playlist()[0].audio == nullptr);
    Rig r(*degraded);
    r.e->transport().play();
    const Out o = r.run({512});
    CHECK(firstNonZero(o.l) < 0);  // silence, no crash
    const std::string again = (dir / "again.wezt").string();
    CHECK(degraded->save(again, &err));
    std::ifstream f(again);
    std::stringstream ss; ss << f.rdbuf();
    const auto a = state.find("state="), b = ss.str().find("state=");
    CHECK(a != std::string::npos && b != std::string::npos && state.substr(a, 15) == ss.str().substr(b, 15));  // plugin settings survived
  }

  // Damaged files give a clear error rather than a half-built session.
  auto expectFail = [&](const std::string& content, const char* what) {
    const std::string p = (dir / "bad.wezt").string();
    { std::ofstream f(p, std::ios::binary); f << content; }
    std::string e;
    CHECK(Session::load(p, factory, &e) == nullptr);
    if (e.empty()) std::printf("  (no message for %s)\n", what);
    CHECK(!e.empty());
  };
  expectFail(state.substr(0, state.size() - 5), "truncated");
  expectFail("hello world\n", "not a session");
  expectFail("", "empty");
  expectFail("WEZT-SESSION 99\nrate=48000 channels=2 block=256\nend\n", "newer");
  expectFail("WEZT-SESSION 1\nrate=0 channels=2 block=256\nend\n", "bad header");
  expectFail("WEZT-SESSION 1\nrate=48000 channels=99 block=256\nend\n", "bad channels");
  CHECK(Session::load((dir / "nope.wezt").string(), factory, &err) == nullptr);
  {  // unknown lines and keys from a future version are ignored
    std::string p = (dir / "future.wezt").string();
    std::ofstream f(p);
    f << "WEZT-SESSION 1\nrate=44100 channels=2 block=128\nmaster name=Master gainDb=0 pan=0 mute=0 shiny=yes\nwidget a=b\ntrack name=T gainDb=-1 mono=1 out=0 future=1\nend\n";
    f.close();
    auto fl = Session::load(p, factory, &err);
    CHECK(fl && fl->trackCount() == 1);
  }
}

static void testBounce() {
  const fs::path dir = tmpDir("bounce");
  Session s(48000, 2, 128);
  const auto t = s.addTrack("T", true);
  s.addClip(t, clipOf(impulses(300, {{0, 1.f}, {50, 0.5f}}), 1000));
  const auto fx = s.addBus("Echo");
  s.addSend(t, fx, -6.f);
  CHECK(s.addInsert(t, std::make_unique<FakePlugin>(1.f, 37), fakeDesc()));
  const std::string path = (dir / "mix.wav").string();
  const BounceResult b = s.bounce(path, 900, 1300, 150);
  CHECK(b.ok && b.frames == 550);
  CHECK(b.peak > 0.5f);
  io::WavReader rd;
  std::string err;
  CHECK(rd.open(path, &err));
  CHECK(rd.info().frames == 550 && rd.info().channels == 2 && rd.info().timeReference == 900);
  std::vector<float> l(550), rr(550);
  float* p[2] = {l.data(), rr.data()};
  CHECK(rd.read(0, 550, p) == 550);
  // Impulse at 1000 shows up at 100 (+ plugin latency 37 + compensation... all paths are aligned).
  CHECK(firstNonZero(l) == 100 + 37);

  // Same as playing it live, sample for sample.
  Rig r(s);
  r.e->transport().locate(900);
  r.e->transport().play();
  const Out live = r.run(sizesFor(550));
  bool same = true;
  for (std::size_t i = 0; i < 550; ++i) same = same && live.l[i] == l[i] && live.r[i] == rr[i];
  CHECK(same);

  CHECK(!s.bounce(path, 100, 100).ok);
  CHECK(!s.bounce((dir / "no" / "such" / "dir.wav").string(), 0, 100).ok);
  // The session still plays after a bounce, through the original engine.
  r.e->transport().locate(900);
  const Out again = r.run({550});
  CHECK(firstNonZero(again.l) == 137);
}

static void testStructureEdits() {
  Session s(48000, 2, 128);
  const auto a = s.addTrack("A", true), b = s.addTrack("B", true);
  s.addClip(a, clipOf(mono(3000, 0.5f), 0));
  s.addClip(b, clipOf(mono(3000, 0.5f), 0));
  s.track(a)->strip->setPan(-1.f); s.track(b)->strip->setPan(-1.f);
  Rig r(s);
  r.e->transport().play();
  Out o = r.run({128});
  CHECK(s.trackCount() == 2);
  CHECK(s.removeTrack(a));
  CHECK(!s.removeTrack(a));
  CHECK(s.trackCount() == 1 && s.track(a) == nullptr);
  o = r.run({256});
  NEAR(o.l[255], 0.5, 1e-5);
  const auto c = s.addTrack("C", true);
  CHECK(c != a && c != b);  // ids are never reused
  CHECK(!s.removeBus(kMaster));
  CHECK(s.addSend(99, kMaster) == -1);
  CHECK(!s.setTrackOutput(b, 42));
  // Clips can be removed.
  const int id = s.addClip(b, clipOf(mono(100, 1.f), 0));
  CHECK(s.removeClip(b, id) && !s.removeClip(b, id));
}

static void testDuplicateTrack() {
  Session s(48000, 2, 128);
  const auto fx = s.addBus("Echo");
  const auto a = s.addTrack("Drums", true);
  s.addClip(a, clipOf(impulses(300, {{0, 1.f}}), 1000));
  s.track(a)->strip->setGainDb(-6.f);
  s.track(a)->strip->setPan(-1.f);
  s.track(a)->strip->volumeLane().add(0, -3.f);
  s.addSend(a, fx, -9.f, true);
  CHECK(s.addInsert(a, std::make_unique<FakePlugin>(0.5f, 10), fakeDesc()));
  const auto before = s.trackCount();
  const auto factory = [](const plugins::PluginDescriptor&) -> std::unique_ptr<plugins::IPluginInstance> { return std::make_unique<FakePlugin>(1.f, 10); };
  const auto d = s.duplicateTrack(a, factory);
  CHECK(d >= 0 && d != a && s.trackCount() == before + 1);
  Track* t = s.track(d);
  CHECK(t && t->name == "Drums dup" && t->mono);
  NEAR(t->strip->gainDb(), -6.0, 1e-6);
  NEAR(t->strip->pan(), -1.0, 1e-6);
  CHECK(t->strip->volumeLane().points().size() == 1);
  CHECK(t->sends.size() == 1 && t->sends[0].bus == fx && t->sends[0].preFader);
  CHECK(t->source->playlist().size() == 1 && !t->source->armed());
  CHECK(t->inserts.size() == 1 && t->inserts[0].instance);
  // The copy got the original's settings (0.5), not the factory default (1.0).
  CHECK(static_cast<FakePlugin*>(t->inserts[0].instance)->gain() == 0.5f);
  // Without a factory the insert is kept as a placeholder with its settings, and a save keeps it.
  const auto d2 = s.duplicateTrack(a);
  CHECK(d2 >= 0 && s.track(d2)->inserts.size() == 1 && !s.track(d2)->inserts[0].instance && !s.track(d2)->inserts[0].offlineState.empty());
  // The copy is independent: editing it leaves the original alone.
  t->strip->setGainDb(0.f);
  NEAR(s.track(a)->strip->gainDb(), -6.0, 1e-6);
  CHECK(s.removeTrack(d));
  CHECK(s.track(a) != nullptr);
  CHECK(s.duplicateTrack(1234) == -1);
  // And it still plays: original and copy both reach the master.
  Rig r(s);
  r.e->transport().locate(900);
  r.e->transport().play();
  const Out o = r.run({400});
  CHECK(firstNonZero(o.l) >= 0 || firstNonZero(o.r) >= 0);
}

// A plug-in that writes NaN is muted by the engine, and the session reports it so the UI can say which one.
class NanPlugin : public plugins::IPluginInstance {
 public:
  void prepare(double, std::size_t) override {}
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    for (std::size_t c = 0; c < out.channels; ++c)
      for (std::size_t i = 0; i < out.frames; ++i) out.channel(c)[i] = bad ? std::numeric_limits<float>::quiet_NaN() : in.channel(c)[i];
  }
  std::size_t parameterCount() const override { return 0; }
  std::vector<std::uint8_t> saveState() const override { return {}; }
  bool loadState(const std::vector<std::uint8_t>&) override { return false; }
  bool bad = true;
};

static void testPluginFaultReport() {
  Session s(48000, 2, 128);
  const auto t = s.addTrack("T", true);
  s.addClip(t, clipOf(impulses(20000, {{0, 1.f}}), 0));
  auto np = std::make_unique<NanPlugin>(); NanPlugin* raw = np.get();
  auto d = fakeDesc(); d.name = "Flaky";
  CHECK(s.addInsert(t, std::move(np), d));
  CHECK(s.pluginFaults().empty());
  Rig r(s);
  r.e->transport().play();
  const Out o = r.run(std::vector<unsigned>(8, 128));
  for (float v : o.l) CHECK(std::isfinite(v));            // the speakers never see it
  auto f = s.pluginFaults();
  CHECK(f.size() == 1 && f[0].name == "Flaky" && f[0].quarantined && f[0].insert == 0 && f[0].badBlocks >= 4);
  raw->bad = false;
  CHECK(s.reenablePlugin(t, 0));
  CHECK(!s.pluginFaults().empty() && !s.pluginFaults()[0].quarantined);
  CHECK(!s.reenablePlugin(t, 5));
}

static void testStripLaneEdit() {
  Session s(48000, 2, 128);
  const auto t = s.addTrack("A", true);
  edit::UndoStack undo;
  const std::vector<session::AutomationPoint> pts{{0, -6.f}, {48000, 3.f}, {96000, 99.f}};   // the last one is above the ceiling
  CHECK(s.setStripLaneUndoable(undo, t, 0, pts, "draw"));
  const auto& lane = s.track(t)->strip->volumeLane();
  CHECK(lane.points().size() == 3);
  CHECK(lane.points()[2].value <= 12.f);                 // clamped
  CHECK(std::fabs(lane.valueAt(24000, 0.f) - (-1.5f)) < 1e-4f);
  CHECK(undo.canUndo());
  undo.undo();
  CHECK(lane.points().empty());                          // undo puts the old (empty) lane back
  undo.redo();
  CHECK(lane.points().size() == 3);
  CHECK(!s.setStripLane(t, 7, pts) && !s.setStripLane(99, 0, pts));
  CHECK(s.setStripLane(t, 2, {{10, 0.7f}}) && s.track(t)->strip->muteLane().points()[0].value == 1.f);   // mute lane is on/off
}

static void testHalfSpeedPlayback() {
  Session s(48000, 2, 128);
  const auto t = s.addTrack("Ramp", true, {0});
  s.track(t)->strip->setPan(-1.f);
  auto buf = std::make_shared<AudioBuffer>(1, 8000);
  for (std::size_t i = 0; i < 8000; ++i) buf->channel(0)[i] = static_cast<float>(i) / 10000.f;
  Clip c; c.audio = buf; c.start = 0; c.length = 8000; c.name = "ramp";
  s.addClip(t, c);
  Rig r(s);
  r.e->playHalfSpeed();
  const Out o = r.run(std::vector<unsigned>(8, 128));        // 1024 output frames = 512 timeline frames
  NEAR(o.l[400], 200.0 / 10000.0, 2e-4);                      // output frame 400 shows timeline frame 200
  NEAR(o.l[401], 200.5 / 10000.0, 2e-4);                      // and the one in between is blended
  NEAR(o.l[1000], 500.0 / 10000.0, 2e-4);
}

static void testLoopRecording() {
  const fs::path dir = tmpDir("looprec");
  auto input = [](std::int64_t k, unsigned c) { return 0.5f * std::sin(0.01f * static_cast<float>(k) + static_cast<float>(c)); };
  Session s(48000, 2, 128);
  s.setRecordFolder((dir / "Audio Files").string());
  const auto t = s.addTrack("Vox", true, {1});
  s.track(t)->source->arm(true);
  Rig r(s, 2, 32, 16);
  r.e->startLoopRecord(1000, 1512);                   // a loop of four blocks
  r.run(std::vector<unsigned>(9, 128), input);        // two full passes and a bit of a third
  r.e->transport().stop();
  r.run({128}, input);
  std::string err;
  CHECK(s.commitTakes(&err) == 3);                    // every pass is its own take
  const auto list = s.track(t)->source->playlist();
  CHECK(list.size() == 3);
  if (list.size() == 3) {
    CHECK(list[0].length == 512 && list[1].length == 512 && list[2].length == 128);
    CHECK(list[0].start == list[1].start && list[1].start == list[2].start);
  }
  CHECK(s.track(t)->source->droppedBlocks() == 0);
  fs::remove_all(dir);
}

static void testProcessClip() {
  Session s(48000, 2, 128);
  const std::string dir = (fs::temp_directory_path() / "wezt_procclip").string();
  fs::remove_all(dir);
  s.setRecordFolder(dir);
  const auto t = s.addTrack("Vox", true, {0});
  AudioBuffer b(1, 8000);
  for (std::size_t i = 0; i < 8000; ++i) b.channel(0)[i] = static_cast<float>(i) / 16000.f;   // ramp 0..0.5
  Clip c = clipOf(std::make_shared<AudioBuffer>(std::move(b)), 100);
  c.offset = 1000; c.length = 4000; c.gainDb = -3.f; c.name = "Take";
  const int id = s.addClip(t, c);
  std::string err;
  CHECK(s.processClip(t, id, "rev", [](AudioBuffer& x, double, std::string*) { for (std::size_t ch = 0; ch < x.channels; ++ch) std::reverse(x.channel(ch), x.channel(ch) + x.frames); return true; }, &err));
  auto list = s.track(t)->source->playlist();
  CHECK(list.size() == 1);
  if (list.size() == 1 && list[0].audio) {
    const Clip& k = list[0];
    CHECK(k.start == 100 && k.offset == 0 && k.length == 4000 && k.gainDb == -3.f);
    NEAR(k.audio->channel(0)[0], 4999.0 / 16000.0, 1e-4);   // the last frame the clip played comes first now
    NEAR(k.audio->channel(0)[3999], 1000.0 / 16000.0, 1e-4);
    CHECK(fs::exists(k.file));
  }
  CHECK(!s.processClip(t, id, "x", [](AudioBuffer&, double, std::string* e) { if (e) *e = "refused"; return false; }, &err) && err == "refused");
  CHECK(!s.processClip(t, 9999, "x", [](AudioBuffer&, double, std::string*) { return true; }, &err));
}

static void testConsolidateAndRenderTrack() {
  Session s(48000, 2, 128);
  const std::string dir = (fs::temp_directory_path() / "wezt_consolidate").string();
  fs::remove_all(dir);
  s.setRecordFolder(dir);
  const auto t = s.addTrack("Gtr", true, {0});
  Clip c = clipOf(mono(10000, 0.5f), 1000);
  c.gainDb = -6.0206f;                                      // -6 dB: 0.5 -> 0.25
  c.fadeIn = 100;
  s.addClip(t, c);
  s.addClip(t, clipOf(mono(2000, 0.5f), 20000));            // a second clip outside the range
  std::string err;
  const int id = s.consolidate(t, 500, 12000, &err);
  CHECK(id >= 0);
  const auto list = s.track(t)->source->playlist();
  CHECK(list.size() == 2);
  const Clip* cons = nullptr;
  for (const auto& k : list) if (k.id == id) cons = &k;
  CHECK(cons && cons->start == 500 && cons->length == 11500);
  if (cons && cons->audio) {
    const float* d = cons->audio->channel(0);
    NEAR(d[300], 0.0, 1e-6);                                // before the clip: silence
    NEAR(d[550], 0.25 * 0.5, 1e-3);       // halfway into the 100-frame fade (clip starts at 1000)
    NEAR(d[5000], 0.25, 1e-4);                              // gain applied
  }
  CHECK(s.consolidate(t, 15000, 16000, &err) == -1);         // nothing in range

  // rendering the track alone onto a new track
  const auto other = s.addTrack("Other", true, {0});
  s.addClip(other, clipOf(mono(12000, 0.9f), 0));
  s.track(t)->strip->setPan(-1.f);
  const auto nt = s.renderTrackToNewTrack(t, 0, 12000, true, &err);
  CHECK(nt >= 0);
  if (nt >= 0) {
    const auto nl = s.track(nt)->source->playlist();
    CHECK(nl.size() == 1);
    float peak = 0;
    if (nl.size() == 1 && nl[0].audio) for (std::size_t i = 0; i < nl[0].audio->frames && i < 12000; ++i) peak = std::max(peak, std::fabs(nl[0].audio->channel(0)[i]));
    CHECK(peak > 0.2f && peak < 0.3f);                      // only the 0.25 track, not "Other" (0.9)
    CHECK(s.track(t)->strip->muted());
    CHECK(!s.anySolo());                                    // the solos were put back
  }
  fs::remove_all(dir);
}

int main() {
  testHalfSpeedPlayback();
  testLoopRecording();
  testConsolidateAndRenderTrack();
  testProcessClip();
  testStripLaneEdit();
  testPluginFaultReport();
  testPlaybackAndPan();
  testMuteSolo();
  testSends();
  testLatencyAlignment();
  testAutomation();
  testFadesAndOverlap();
  testRecording();
  testRecordingErrors();
  testSaveLoad();
  testBounce();
  testStructureEdits();
  testDuplicateTrack();
  std::printf(failures ? "FAILED\n" : "session tests passed\n");
  return failures;
}
