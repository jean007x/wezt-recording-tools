// Engine and graph behaviour: variable callback sizes, device/engine mismatches, sample-accurate
// loops, input monitoring, plugin delay compensation, nodes added/removed after prepare.
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "audio/audio_device.h"
#include "engine/engine.h"
#include "engine/graph.h"

using namespace wezt;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Outputs the timeline position of every sample: proves what the engine tells processors.
class PositionRamp : public Processor {
 public:
  explicit PositionRamp(const RtContext* c) : ctx_(c) {}
  void process(const AudioBuffer&, AudioBuffer& out) override {
    for (std::size_t c = 0; c < out.channels; ++c)
      for (std::size_t i = 0; i < out.frames; ++i) out.channel(c)[i] = static_cast<float>(ctx_->chunkStart + static_cast<std::int64_t>(i));
  }
 private:
  const RtContext* ctx_;
};

// Passes device input channel 0 to every output channel (input monitoring).
class InputMonitor : public Processor {
 public:
  explicit InputMonitor(const RtContext* c) : ctx_(c) {}
  void process(const AudioBuffer&, AudioBuffer& out) override {
    const float* in = ctx_->inputChannel(0);
    for (std::size_t c = 0; c < out.channels; ++c)
      for (std::size_t i = 0; i < out.frames; ++i) out.channel(c)[i] = in[i];
  }
 private:
  const RtContext* ctx_;
};

// 1.0 on the very first sample after prepare(), silence after.
class Impulse : public Processor {
 public:
  void prepare(double, std::size_t) override { done_ = false; }
  void process(const AudioBuffer&, AudioBuffer& out) override {
    out.clearValid();
    if (!done_ && out.frames) { for (std::size_t c = 0; c < out.channels; ++c) out.channel(c)[0] = 1.f; done_ = true; }
  }
 private:
  bool done_ = false;
};

// Delays by `n` samples and says so, like a look-ahead plugin.
class Delay : public Processor {
 public:
  explicit Delay(std::size_t n) : n_(n) {}
  void prepare(double, std::size_t) override { d_.setDelay(n_, 2); }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    for (std::size_t c = 0; c < out.channels; ++c) std::copy(in.channel(c), in.channel(c) + out.frames, out.channel(c));
    d_.process(out);
  }
  std::size_t latencySamples() const override { return n_; }
 private:
  std::size_t n_;
  DelayLine d_;
};

struct Rig {
  Graph g;
  std::unique_ptr<Engine> e;
  audio::NullDevice dev;
  explicit Rig(std::size_t block = 128, std::size_t ch = 2, double rate = 48000.0) { e = std::make_unique<Engine>(g, block, ch, rate, 2); }
  bool start(unsigned devIn, unsigned devOut, unsigned devRate = 48000, unsigned inLat = 0, unsigned outLat = 0) {
    audio::AudioConfig c;
    c.sampleRate = devRate; c.inChannels = devIn; c.outChannels = devOut; c.inputLatencyFrames = inLat; c.outputLatencyFrames = outLat;
    std::string err;
    return dev.open(c, e.get(), &err) && dev.start(&err);
  }
};

// Runs callbacks of the given sizes and returns channel 0 of everything produced.
static std::vector<float> runSizes(Rig& r, unsigned devOut, const std::vector<unsigned>& sizes, float input = 0.f, unsigned devIn = 0) {
  std::vector<float> all;
  for (unsigned n : sizes) {
    std::vector<std::vector<float>> in(devIn, std::vector<float>(n, input)), out(devOut, std::vector<float>(n, -9.f));
    std::vector<const float*> ip;
    std::vector<float*> op;
    for (auto& v : in) ip.push_back(v.data());
    for (auto& v : out) op.push_back(v.data());
    r.e->process(devIn ? ip.data() : nullptr, op.data(), n);
    all.insert(all.end(), out[0].begin(), out[0].end());
  }
  return all;
}

static void testVariableCallbackSizes() {
  auto make = [](std::vector<unsigned> sizes) {
    Rig r(128);
    r.g.setOutput(r.g.add(std::make_unique<Sine>(997.0)));
    r.g.prepare(48000, 2, 128);
    CHECK(r.start(0, 2));
    r.e->transport().play();
    return runSizes(r, 2, sizes);
  };
  const auto whole = make({1000});
  const auto odd = make({1, 7, 128, 300, 3, 561});  // sums to 1000; some exceed the engine block
  CHECK(whole.size() == 1000 && odd.size() == 1000);
  bool same = true;
  for (std::size_t i = 0; i < 1000; ++i) same = same && whole[i] == odd[i];
  CHECK(same);  // bit-identical regardless of how the device slices time
  float peak = 0;
  for (float v : whole) peak = std::fmax(peak, std::fabs(v));
  CHECK(peak > 0.99f);
}

static void testTransportAccuracy() {
  Rig r(128);
  auto ramp = r.g.add(std::make_unique<PositionRamp>(r.e->context()));
  r.g.setOutput(ramp);
  CHECK(r.start(0, 2));
  // stopped: renders but does not advance
  auto s = runSizes(r, 2, {100});
  CHECK(r.e->transport().position() == 0 && s[50] == 50.f);
  r.e->transport().play();
  s = runSizes(r, 2, {100, 100, 37});
  CHECK(r.e->transport().position() == 237);  // exact, not rounded to the engine block size
  CHECK(s[0] == 0.f && s[236] == 236.f);

  // sample-accurate loop: 100..300 (200 long) with callbacks that do not divide it
  r.e->transport().stop();
  r.e->transport().setLoop(100, 300);
  r.e->transport().locate(250);
  r.e->transport().play();
  s = runSizes(r, 2, {128, 128, 128});
  bool ok = true;
  float expect = 250;
  for (float v : s) { ok = ok && v == expect; expect += 1; if (expect >= 300) expect = 100; }
  CHECK(ok);  // 250..299, then straight back to 100: no overshoot, no gap
  CHECK(r.e->transport().position() == 100 + (384 - 50) % 200);
}

static void testInputMonitoring() {
  Rig r(64);
  r.g.setOutput(r.g.add(std::make_unique<InputMonitor>(r.e->context())));
  CHECK(r.start(2, 2));
  auto s = runSizes(r, 2, {64}, 0.3f, 2);  // transport stopped
  CHECK(std::fabs(s[10] - 0.3f) < 1e-7f);  // you can hear what you are about to record
  CHECK(r.e->transport().position() == 0);
}

static void testDeviceMismatch() {
  {  // device has FEWER output channels than the engine: used to overflow the heap
    Rig r(64, 2);
    r.g.setOutput(r.g.add(std::make_unique<Sine>(100.0)));
    CHECK(r.start(0, 1));
    r.e->transport().play();
    auto s = runSizes(r, 1, {64, 64});
    CHECK(s.size() == 128 && std::fabs(s[25]) > 0.1f);
  }
  {  // device has MORE: extra channels are silent, not garbage
    Rig r(64, 2);
    r.g.setOutput(r.g.add(std::make_unique<Sine>(100.0)));
    CHECK(r.start(0, 8));
    r.e->transport().play();
    std::vector<std::vector<float>> out(8, std::vector<float>(64, -9.f));
    std::vector<float*> op;
    for (auto& v : out) op.push_back(v.data());
    r.e->process(nullptr, op.data(), 64);
    CHECK(out[0][25] != 0.f && out[1][25] != 0.f);
    for (int c = 2; c < 8; ++c) CHECK(out[c][0] == 0.f && out[c][63] == 0.f);
  }
  {  // device runs at another sample rate: the graph follows it
    Rig r(128, 2, 48000.0);
    r.g.setOutput(r.g.add(std::make_unique<Sine>(441.0)));
    CHECK(r.start(0, 2, 44100));
    CHECK(r.e->sampleRateChanged() && r.e->sampleRate() == 44100.0);
    r.e->transport().play();
    auto s = runSizes(r, 2, {128});
    CHECK(std::fabs(s[25] - 1.f) < 1e-3f);  // 441 Hz at 44.1 kHz: a quarter period is exactly 25 frames
  }
  {  // fewer input channels than a processor asks for: silence, never out of range
    Rig r(64);
    r.g.setOutput(r.g.add(std::make_unique<InputMonitor>(r.e->context())));
    CHECK(r.start(0, 2));
    auto s = runSizes(r, 2, {64});
    CHECK(s[5] == 0.f);
  }
  {  // device latency reaches processors for record compensation
    Rig r(64);
    r.g.setOutput(r.g.add(std::make_unique<Sine>(100.0)));
    CHECK(r.start(2, 2, 48000, 96, 160));
    CHECK(r.e->context()->recordLatencyFrames == 256);
  }
}

static void testGraphEditing() {
  {  // a node added after prepare() is prepared and rendered (used to produce an empty buffer)
    Graph g;
    auto a = g.add(std::make_unique<Sine>(100));
    g.setOutput(a);
    g.prepare(48000, 2, 64);
    auto late = g.add(std::make_unique<Gain>(0.5f));
    CHECK(g.connect(a, late));
    g.setOutput(late);
    AudioBuffer out;
    g.render(out, 64);
    CHECK(out.frames == 64 && out.peak() > 0.1f && out.peak() <= 0.5f + 1e-6f);
  }
  {  // remove, duplicate edges, cycles
    Graph g;
    auto a = g.add(std::make_unique<Sine>(100)), b = g.add(std::make_unique<Gain>(1.f)), c = g.add(std::make_unique<Gain>(1.f));
    CHECK(g.connect(a, b) && g.connect(a, b) && g.connect(b, c));
    CHECK(!g.connect(c, a) && !g.connect(b, a) && !g.connect(a, a));
    CHECK(g.nodeCount() == 3);
    CHECK(g.remove(b) && g.nodeCount() == 2 && !g.remove(b));
    g.setOutput(c);
    g.prepare(48000, 2, 32);
    AudioBuffer out;
    g.render(out, 32);
    CHECK(out.peak() == 0.f);  // c lost its input
    CHECK(g.connect(a, c));
    g.render(out, 32);
    CHECK(out.peak() > 0.f);
  }
  {  // gain changes ramp instead of stepping (no zipper clicks)
    Graph g;
    auto s = g.add(std::make_unique<Impulse>());  // placeholder source: constant 1.0 below
    (void)s;
    auto src = g.add(std::make_unique<Sine>(0.0));  // sine at 0 Hz is constant 0; use gain on a DC-like path instead
    (void)src;
    Gain gain(1.f);
    gain.setGain(0.f);
    AudioBuffer in(1, 100), out(1, 100);
    for (std::size_t i = 0; i < 100; ++i) in.channel(0)[i] = 1.f;
    gain.process(in, out);
    CHECK(out.channel(0)[0] > 0.9f && std::fabs(out.channel(0)[99]) < 1e-6f);   // not exactly 0 where the compiler fuses multiply-add (arm64)
    bool mono = true;
    for (int i = 1; i < 100; ++i) mono = mono && out.channel(0)[i] <= out.channel(0)[i - 1];
    CHECK(mono);  // smooth fade, no single-sample jump
  }
}

static void testDelayCompensation() {
  Graph g;
  auto imp = g.add(std::make_unique<Impulse>());
  auto del = g.add(std::make_unique<Delay>(64));
  auto mix = g.add(std::make_unique<Gain>(1.f));
  CHECK(g.connect(imp, del) && g.connect(del, mix) && g.connect(imp, mix));
  g.setOutput(mix);
  g.prepare(48000, 2, 100);
  CHECK(g.outputLatency() == 64);
  AudioBuffer out;
  std::vector<float> all;
  for (std::size_t n : {37u, 91u, 72u}) {  // odd chunk sizes across the delay boundary
    g.render(out, n);
    all.insert(all.end(), out.channel(0), out.channel(0) + n);
  }
  CHECK(all.size() == 200);
  CHECK(all[0] == 0.f);       // the direct path no longer arrives early
  CHECK(all[64] == 2.f);      // both paths land on the same sample
  float rest = 0;
  for (std::size_t i = 0; i < all.size(); ++i) if (i != 64) rest += std::fabs(all[i]);
  CHECK(rest == 0.f);
}

// A plugin that emits NaN/Inf must not reach the hardware.
class Poison : public Processor {
 public:
  void process(const AudioBuffer&, AudioBuffer& out) override {
    for (std::size_t c = 0; c < out.channels; ++c)
      for (std::size_t i = 0; i < out.frames; ++i) out.channel(c)[i] = i % 3 == 0 ? std::nanf("") : (i % 3 == 1 ? INFINITY : 0.5f);
  }
};
static void testNonFiniteOutput() {
  Rig r(64);
  r.g.setOutput(r.g.add(std::make_unique<Poison>()));
  r.g.prepare(48000, 2, 64);
  CHECK(r.start(0, 2));
  r.e->transport().play();
  const auto v = runSizes(r, 2, {64});
  bool ok = true;
  for (std::size_t i = 0; i < v.size(); ++i) ok = ok && std::isfinite(v[i]) && (i % 3 == 2 ? v[i] == 0.5f : v[i] == 0.f);
  CHECK(ok);
}

int main() {
  testNonFiniteOutput();
  testVariableCallbackSizes();
  testTransportAccuracy();
  testInputMonitoring();
  testDeviceMismatch();
  testGraphEditing();
  testDelayCompensation();
  std::printf(failures ? "FAILED\n" : "engine tests passed\n");
  return failures;
}
