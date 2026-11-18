// Runs Wezt's real CoreAudio backend source against the CoreAudio simulator (tests/sim/macos).
// Scope: backend logic (device choice, aggregate, hog mode, fallback, channel mapping, latency).
// It cannot show how a real Mac behaves.
#include <cmath>
#include <cstdio>
#include <memory>

#include "audio/audio_device.h"
#include "engine/engine.h"
#include "sim_coreaudio.h"

using namespace wezt;
using namespace wezt::sim::mac;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void hardware() {
  reset();
  addDevice("builtin-out", "MacBook Speakers", 0, 2);
  addDevice("builtin-mic", "MacBook Microphone", 1, 0);
  addDevice("rme", "Audio Interface", 8, 8);
  device("rme")->latency = 24;
  device("rme")->safety = 32;
  device("rme")->streamLatency = 8;
  setDefault(true, "builtin-out");
  setDefault(false, "builtin-mic");
}

struct Rig {
  Graph g;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<audio::AudioDevice> dev;
  Rig(unsigned channels) {
    auto s = g.add(std::make_unique<Sine>(1000.0));
    g.setOutput(s);
    engine = std::make_unique<Engine>(g, 128, channels, 48000.0, 2);
    dev = audio::createPlatformDevice();
  }
};

static float peak(const std::vector<float>& v) {
  float p = 0;
  for (float x : v) p = std::fmax(p, std::fabs(x));
  return p;
}

static void testListing() {
  hardware();
  auto list = audio::listDevices();
  CHECK(list.size() == 3);
  int defOut = 0, defIn = 0;
  for (auto& d : list) {
    defOut += d.isDefaultOutput;
    defIn += d.isDefaultInput;
    if (d.id == "rme") CHECK(d.inputChannels == 8 && d.outputChannels == 8 && d.name == "Audio Interface");
  }
  CHECK(defOut == 1 && defIn == 1);
  CHECK(liveCFObjects() == 0);
}

static void testAggregateForSeparateDevices() {
  hardware();
  Rig r(2);
  audio::AudioConfig cfg;
  cfg.bufferFrames = 256;
  std::string err;
  CHECK(r.dev->open(cfg, r.engine.get(), &err));
  CHECK(r.dev->start(&err));
  // Speakers + mic are different devices: a private aggregate must be created.
  CHECK(aggregateCount() == 1);
  for (auto uid : {"builtin-out", "builtin-mic"}) CHECK(device(uid) != nullptr);
  const FakeDevice* agg = nullptr;
  for (AudioDeviceID id = 1; id < 40; ++id)
    if (auto* d = deviceById(id)) if (d->isAggregate) agg = d;
  CHECK(agg && agg->isPrivate && agg->masterUid == "builtin-out" && agg->subDevices.size() == 2);
  if (agg) {
    CHECK(agg->subDevices[0].first == "builtin-out" && agg->subDevices[0].second == 0);  // clock master: no drift correction
    CHECK(agg->subDevices[1].first == "builtin-mic" && agg->subDevices[1].second == 1);  // follower: drift corrected
  }
  CHECK(audio::listDevices().size() == 3);  // private aggregate stays hidden

  r.engine->transport().record();
  pump(8);
  r.dev->stop();
  CHECK(aggregateCount() == 0);
  CHECK(liveCFObjects() == 0);  // no CoreFoundation leaks

  auto out = lastOutput();
  CHECK(out.size() == 2 && peak(out[0]) > 0.9f && peak(out[1]) > 0.9f);
  const auto actual = r.dev->actualConfig();
  CHECK(actual.inChannels == 1 && actual.bufferFrames == 256 && !actual.exclusive);
  CHECK(r.engine->recordedFrames() >= 8 * 128);
  wezt::AudioBuffer rec = r.engine->takeRecording();
  CHECK(rec.frames > 0 && std::fabs(rec.channel(0)[5] - 0.1f) < 1e-6f);  // the mic's channel 0
}

static void testInterfaceExclusive() {
  hardware();
  Rig r(2);
  audio::AudioConfig cfg;
  cfg.outputDeviceId = "rme";
  cfg.bufferFrames = 128;
  cfg.exclusive = true;
  std::string err;
  CHECK(r.dev->open(cfg, r.engine.get(), &err));
  CHECK(r.dev->start(&err));
  CHECK(aggregateCount() == 0);  // the interface serves input and output itself
  CHECK(device("rme")->hogOwner == getpid());
  const auto a = r.dev->actualConfig();
  CHECK(a.exclusive && a.inChannels == 2 && a.bufferFrames == 128);
  CHECK(a.outputLatencyFrames == 24 + 32 + 8 + 128);
  CHECK(a.inputLatencyFrames == 24 + 32 + 8 + 128);
  CHECK(r.dev->backendName().find("exclusive") != std::string::npos);
  r.engine->transport().play();
  pump(4);
  CHECK(peak(lastOutput()[0]) > 0.9f);
  r.dev->stop();
  CHECK(device("rme")->hogOwner == -1);  // released on stop
}

static void testExclusiveRefused() {
  hardware();
  device("rme")->hogOwner = 4242;  // another process owns it
  {
    Rig r(2);
    audio::AudioConfig cfg;
    cfg.outputDeviceId = "rme";
    cfg.exclusive = true;
    cfg.fallbackToShared = true;
    std::string err;
    CHECK(r.dev->open(cfg, r.engine.get(), &err));
    CHECK(r.dev->start(&err));
    CHECK(!r.dev->actualConfig().exclusive);
    CHECK(r.dev->notice().find("shared mode") != std::string::npos && r.dev->notice().find("4242") != std::string::npos);
    CHECK(device("rme")->hogOwner == 4242);  // never steal it
    r.dev->stop();
  }
  {
    Rig r(2);
    audio::AudioConfig cfg;
    cfg.outputDeviceId = "rme";
    cfg.exclusive = true;
    cfg.fallbackToShared = false;
    std::string err;
    CHECK(r.dev->open(cfg, r.engine.get(), &err));
    CHECK(!r.dev->start(&err));
    CHECK(err.find("exclusive") != std::string::npos);
    CHECK(device("rme")->hogOwner == 4242);
  }
  hardware();
  device("rme")->refuseHog = true;  // driver says no
  {
    Rig r(2);
    audio::AudioConfig cfg;
    cfg.outputDeviceId = "rme";
    cfg.exclusive = true;
    std::string err;
    CHECK(r.dev->open(cfg, r.engine.get(), &err));
    CHECK(r.dev->start(&err));
    CHECK(!r.dev->actualConfig().exclusive && !r.dev->notice().empty());
    r.dev->stop();
  }
}

static void testChannelMapping() {
  hardware();
  Rig r(4);  // engine wants 4 channels, speakers have 2
  audio::AudioConfig cfg;
  cfg.outChannels = 4;
  std::string err;
  CHECK(r.dev->open(cfg, r.engine.get(), &err));
  CHECK(r.dev->start(&err));
  r.engine->transport().play();
  pump(3);
  CHECK(lastClientOutChannels() == 2);
  CHECK(lastOutput().size() == 2 && peak(lastOutput()[0]) > 0.9f);
  r.dev->stop();
}

static void testErrorsAndClamping() {
  hardware();
  {
    Rig r(2);
    audio::AudioConfig cfg;
    cfg.outputDeviceId = "does-not-exist";
    std::string err;
    CHECK(r.dev->open(cfg, r.engine.get(), &err));
    CHECK(!r.dev->start(&err) && err.find("output device not found") != std::string::npos);
  }
  {
    Rig r(2);
    audio::AudioConfig cfg;
    cfg.inputDeviceId = "builtin-out";  // has no inputs
    std::string err;
    CHECK(r.dev->open(cfg, r.engine.get(), &err));
    CHECK(!r.dev->start(&err) && err.find("input device") != std::string::npos);
  }
  {
    Rig r(2);
    audio::AudioConfig cfg;
    cfg.inChannels = 0;
    cfg.bufferFrames = 8;  // below what the hardware allows: backend must report the real size
    std::string err;
    CHECK(r.dev->open(cfg, r.engine.get(), &err));
    CHECK(r.dev->start(&err));
    CHECK(r.dev->actualConfig().bufferFrames == 64);
    CHECK(aggregateCount() == 0);  // output only: nothing to combine
    r.dev->stop();
  }
  CHECK(liveCFObjects() == 0);
}

int main() {
  testListing();
  testAggregateForSeparateDevices();
  testInterfaceExclusive();
  testExclusiveRefused();
  testChannelMapping();
  testErrorsAndClamping();
  std::printf(failures ? "FAILED\n" : "macOS simulator tests passed\n");
  return failures;
}
