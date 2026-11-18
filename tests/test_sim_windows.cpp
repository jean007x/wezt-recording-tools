// Runs Wezt's real WASAPI backend source against the Windows/WASAPI simulator (tests/sim/windows).
// Scope: backend logic (device choice, shared/exclusive negotiation, format fallback, buffer
// alignment, refusal handling, capture, MMCSS priority, resource cleanup). It cannot show how
// real Windows drivers behave; the GitHub Actions workflow builds and tests on real Windows.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "audio/audio_device.h"
#include "engine/engine.h"
#include "sim_windows_api.h"

using namespace wezt;
using namespace wezt::sim::win;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static void hardware() {
  reset();
  addEndpoint(L"out-main", L"Speakers", false, 2, true);
  addEndpoint(L"mic-main", L"Microphone", true, 2, true);
  addEndpoint(L"iface-out", L"Audio Interface Out", false, 8, false);
  addEndpoint(L"iface-in", L"Audio Interface In", true, 8, false);
}

struct Rig {
  Graph g;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<audio::AudioDevice> dev;
  audio::AudioConfig cfg;
  std::string err;
  Rig() {
    auto s = g.add(std::make_unique<Sine>(1000.0));
    g.setOutput(s);
    engine = std::make_unique<Engine>(g, 128, 2, 48000.0, 2);
    dev = audio::createPlatformDevice();
    cfg.bufferFrames = 256;
    cfg.outChannels = 2;
    cfg.inChannels = 0;
  }
  bool start() {
    return dev->open(cfg, engine.get(), &err) && dev->start(&err);
  }
};

static float peak(const std::vector<float>& v) {
  float p = 0;
  for (float x : v) p = std::fmax(p, std::fabs(x));
  return p;
}

static void expectClean() {
  CHECK(openHandles() == 0);
  CHECK(liveComObjects() == 0);
}

static void testListing() {
  hardware();
  endpoint(L"out-main")->propsKnown = false;                          // Windows did not say
  endpoint(L"iface-out")->allowExclusive = false;                      // user switched it off
  endpoint(L"iface-in")->exclusivePriority = true;
  auto list = audio::listDevices();
  CHECK(list.size() == 4);
  int defOut = 0, defIn = 0;
  for (auto& d : list) {
    defOut += d.isDefaultOutput;
    defIn += d.isDefaultInput;
    if (d.id == "out-main") {
      CHECK(d.name == "Speakers" && d.outputChannels == 2 && !d.exclusiveSettingsKnown);
    }
    if (d.id == "iface-out") {
      CHECK(d.outputChannels == 8 && d.exclusiveSettingsKnown && !d.allowsExclusive);
    }
    if (d.id == "iface-in") CHECK(d.inputChannels == 8 && d.allowsExclusive && d.exclusivePriority);
  }
  CHECK(defOut == 1 && defIn == 1);
  expectClean();
}

static void testSharedPlayback() {
  hardware();
  {
    Rig r;
    CHECK(r.start());
    CHECK(r.dev->backendName() == "WASAPI (shared)");
    r.engine->transport().play();
    waitMs(250);
    r.dev->stop();
    CHECK(r.dev->actualConfig().exclusive == false);
    auto* ep = endpoint(L"out-main");
    CHECK(ep->sharedInitializeCalls == 1 && ep->exclusiveInitializeCalls == 0);
    CHECK(!ep->lastWasExclusive && ep->lastFormat.kind == 0);
    auto out = renderedAudio(L"out-main");
    CHECK(out.size() == 2 && out[0].size() > 2000);
    CHECK(out.size() == 2 && peak(out[0]) > 0.99f && peak(out[0]) <= 1.0001f);
    CHECK(mmcssRegistrations() >= 1 && lastMmcssPriority() == 2);  // "Pro Audio", critical
  }
  expectClean();
}

static void testExclusivePlayback() {
  hardware();
  {
    Rig r;
    r.cfg.exclusive = true;
    r.cfg.outputDeviceId = "iface-out";
    CHECK(r.start());
    CHECK(r.dev->backendName() == "WASAPI (exclusive)");
    CHECK(r.dev->actualConfig().exclusive);
    r.engine->transport().play();
    waitMs(250);
    r.dev->stop();
    auto* ep = endpoint(L"iface-out");
    CHECK(ep->lastWasExclusive && ep->lastFormat.kind == 3 && ep->lastFormat.validBits == 24);  // 24-in-32 first
    CHECK(ep->sharedInitializeCalls == 0);
    auto out = renderedAudio(L"iface-out");
    CHECK(out.size() == 2 && out[0].size() > 2000 && peak(out[0]) > 0.99f);
    CHECK(r.dev->notice().empty());
  }
  expectClean();
}

static void testFormatFallback() {
  hardware();
  {
    Rig r;  // only 16-bit PCM is native: the backend must walk down its candidate list
    r.cfg.exclusive = true;
    r.cfg.outputDeviceId = "iface-out";
    endpoint(L"iface-out")->native = {{1, 16}};
    CHECK(r.start());
    r.engine->transport().play();
    waitMs(150);
    r.dev->stop();
    CHECK(endpoint(L"iface-out")->lastFormat.kind == 1);
    auto out = renderedAudio(L"iface-out");
    CHECK(out.size() == 2 && peak(out[0]) > 0.99f);
  }
  hardware();
  {
    Rig r;  // nothing native at all: fail clearly, or fall back to shared if allowed
    r.cfg.exclusive = true;
    r.cfg.fallbackToShared = false;
    r.cfg.outputDeviceId = "iface-out";
    endpoint(L"iface-out")->native.clear();
    CHECK(!r.start());
    CHECK(has(r.err, "does not support"));
  }
  hardware();
  {
    Rig r;
    r.cfg.exclusive = true;
    r.cfg.outputDeviceId = "iface-out";
    endpoint(L"iface-out")->native.clear();
    CHECK(r.start());
    CHECK(!r.dev->actualConfig().exclusive && has(r.dev->notice(), "using shared mode"));
    r.dev->stop();
  }
  expectClean();
}

static void testAlignment() {
  hardware();
  {
    Rig r;
    r.cfg.exclusive = true;
    r.cfg.outputDeviceId = "iface-out";
    r.cfg.bufferFrames = 100;
    endpoint(L"iface-out")->alignFrames = 128;
    CHECK(r.start());
    auto* ep = endpoint(L"iface-out");
    CHECK(ep->notAlignedErrors == 1 && ep->exclusiveInitializeCalls == 2);  // refused once, retried aligned
    CHECK(ep->lastBufferFrames == 128 && r.dev->actualConfig().bufferFrames == 128);
    r.engine->transport().play();
    waitMs(150);
    r.dev->stop();
    CHECK(renderedAudio(L"iface-out").size() == 2);
  }
  expectClean();
}

static void testRefusals() {
  hardware();
  {
    Rig r;  // "Allow applications to take exclusive control" is off
    r.cfg.exclusive = true;
    r.cfg.fallbackToShared = false;
    endpoint(L"out-main")->allowExclusive = false;
    CHECK(!r.start());
    CHECK(has(r.err, "Allow applications to take exclusive control"));
  }
  hardware();
  {
    Rig r;
    r.cfg.exclusive = true;
    endpoint(L"out-main")->allowExclusive = false;
    CHECK(r.start());
    CHECK(!r.dev->actualConfig().exclusive);
    CHECK(has(r.dev->notice(), "Allow applications to take exclusive control") && has(r.dev->notice(), "using shared mode"));
    CHECK(endpoint(L"out-main")->sharedInitializeCalls >= 1);
    r.dev->stop();
  }
  hardware();
  {
    Rig r;  // another application already owns the device in exclusive mode
    r.cfg.exclusive = true;
    r.cfg.fallbackToShared = false;
    endpoint(L"out-main")->inUseByOther = true;
    CHECK(!r.start());
    CHECK(has(r.err, "in use"));
  }
  hardware();
  {
    Rig r;  // a device id that does not exist
    r.cfg.outputDeviceId = "no-such-device";
    CHECK(!r.start());
    CHECK(has(r.err, "not found"));
  }
  reset();
  {
    Rig r;  // no audio hardware at all
    CHECK(!r.start());
    CHECK(has(r.err, "not found"));
  }
  expectClean();
}

static void testCapture(bool exclusive) {
  hardware();
  {
    Rig r;
    r.cfg.inChannels = 2;
    r.cfg.exclusive = exclusive;
    if (exclusive) {
      r.cfg.outputDeviceId = "iface-out";
      r.cfg.inputDeviceId = "iface-in";
    }
    CHECK(r.start());
    CHECK(r.dev->actualConfig().inChannels == 2);
    r.engine->transport().record();
    waitMs(300);
    r.dev->stop();
    r.engine->transport().stop();
    AudioBuffer rec = r.engine->takeRecording();
    CHECK(rec.frames > 2000 && rec.channels == 2);
    // The simulated input is a constant 0.1 / 0.2. A few leading frames may be zero (FIFO fill).
    std::size_t good = 0;
    for (std::size_t i = 0; i < rec.frames; ++i)
      if (std::fabs(rec.channel(0)[i] - 0.1f) < 1e-3f && std::fabs(rec.channel(1)[i] - 0.2f) < 1e-3f) ++good;
    CHECK(good > rec.frames / 2);
    if (exclusive) {
      CHECK(endpoint(L"iface-in")->lastWasExclusive);
    }
  }
  expectClean();
}

static void testInputFallbackKeepsOutput() {
  hardware();
  {
    Rig r;
    r.cfg.inChannels = 2;
    r.cfg.inputDeviceId = "no-such-mic";
    CHECK(r.start());  // output still works; the notice explains the missing input
    CHECK(r.dev->actualConfig().inChannels == 0);
    CHECK(has(r.dev->notice(), "continuing without input"));
    r.dev->stop();
  }
  expectClean();
}

int main() {
  testListing();
  testSharedPlayback();
  testExclusivePlayback();
  testFormatFallback();
  testAlignment();
  testRefusals();
  testCapture(false);
  testCapture(true);
  testInputFallbackKeepsOutput();
  std::printf(failures ? "FAILED\n" : "windows simulator tests passed\n");
  return failures;
}
