// Loads a real VST3 plugin (pass the path to e.g. the SDK's "again.vst3") and runs it
// inside a Wezt graph through the engine driver.
#include <cmath>
#include <cstdio>
#include <memory>

#include "audio/audio_device.h"
#include "engine/engine.h"
#include "plugins/vst3_host.h"

using namespace wezt;
using namespace wezt::plugins;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static float peakOf(const AudioBuffer& b) {
  float p = 0;
  for (float v : b.data) p = std::fmax(p, std::fabs(v));
  return p;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: wezt_vst3_test <plugin.vst3>  (skipped)\n");
    return 0;
  }
  std::string err;
  {  // a missing file, and a file that is not a plug-in: a clear error, never a crash
    CHECK(!Vst3Instance::load("/definitely/not/here.vst3", &err));
    CHECK(!err.empty());
    err.clear();
    const char* notPlugin = argv[0];  // this test's own executable: a real file, certainly not a VST3 module
    CHECK(!Vst3Instance::load(notPlugin, &err));
    CHECK(!err.empty());
    err.clear();
  }
  auto classes = describeVst3(argv[1], &err);
  CHECK(!classes.empty());
  if (!classes.empty()) std::printf("class: %s by %s\n", classes[0].name.c_str(), classes[0].vendor.c_str());

  auto inst = Vst3Instance::load(argv[1], &err);
  if (!inst) { std::printf("load failed: %s\n", err.c_str()); return 1; }
  auto params = inst->parameters();
  std::printf("%zu parameters\n", params.size());
  CHECK(!params.empty());

  Vst3Instance* plug = inst.get();
  // The universal control surface, read from a real VST3 plugin (not from anything we wrote).
  {
    CHECK(plug->displayName() == plug->name() && !plug->displayName().empty());
    for (const auto& p : params) {
      std::printf("  param %u \"%s\" plain %g..%g unit \"%s\" steps %d text@default \"%s\"\n", p.id, p.name.c_str(), p.min, p.max, p.unit.c_str(), p.steps,
                  plug->parameterText(p.id, p.defaultNormalized).c_str());
      CHECK(p.min <= p.max);
    }
    CHECK(plug->aiTools().empty());  // a VST3 plugin has no wezt.agent/1 tools
  }
  Graph g;
  auto src = g.add(std::make_unique<Sine>(1000.0));
  auto fx = g.add(std::move(inst));
  CHECK(g.connect(src, fx));
  g.setOutput(fx);
  Engine engine(g, 256, 2, 48000.0, 5);  // prepares the graph, which activates the plugin

  AudioBuffer out;

  // Find a parameter called "Gain". The SDK sample's processor starts at gain 0 until the host
  // sends a value, so push the controller's own default first (a full-scale sine in -> peak ~1).
  unsigned gainId = 0;
  bool found = false;
  double gainDefault = 1.0;
  for (auto& p : params)
    if (p.name.find("Gain") != std::string::npos) { gainId = p.id; gainDefault = p.defaultNormalized; found = true; }
  CHECK(found);
  if (found) {
    CHECK(plug->setParameterNormalized(gainId, gainDefault));
    g.render(out);
    const float defaultPeak = peakOf(out);
    std::printf("peak at default gain (%.2f): %f\n", gainDefault, defaultPeak);
    CHECK(defaultPeak > 0.9f);

    CHECK(plug->setParameterNormalized(gainId, 0.0));
    g.render(out);
    const float mutedPeak = peakOf(out);
    std::printf("peak with gain 0: %f\n", mutedPeak);
    CHECK(mutedPeak < 0.001f);
    plug->setParameterNormalized(gainId, 0.5);
    g.render(out);
    const float halfPeak = peakOf(out);
    std::printf("peak with gain 0.5: %f\n", halfPeak);
    CHECK(halfPeak > 0.001f && halfPeak < defaultPeak);
  }

  if (found) {   // the audio-thread entry used by automation
    CHECK(plug->automateParameter(gainId, 0.0));
    g.render(out);
    CHECK(peakOf(out) < 0.001f);
    CHECK(plug->automateParameter(gainId, gainDefault));
    g.render(out);
    CHECK(peakOf(out) > 0.9f);
  }

  if (found) {
    // A burst of edits: repeats collapse to the latest value, and edits to more than 64 different
    // parameters spill into the next block instead of growing the list on the audio thread.
    plug->setParameterNormalized(gainId, gainDefault);
    g.render(out);
    for (int i = 0; i < 100; ++i) plug->setParameterNormalized(gainId, i % 2 ? 1.0 : 0.25);  // ends at 1.0
    g.render(out);
    CHECK(peakOf(out) > 0.9f);
    for (unsigned id = 1000000; id < 1000100; ++id) plug->setParameterNormalized(id, 0.5);  // 100 other ids
    plug->setParameterNormalized(gainId, 0.0);                                              // queued behind them
    g.render(out);
    g.render(out);
    CHECK(peakOf(out) < 0.001f);
  }

  auto state = plug->saveState();
  CHECK(!state.empty());
  CHECK(plug->loadState(state));
  CHECK(!plug->loadState({}));                                         // empty: refused
  {  // damaged state: the plug-in may accept or refuse it, the host must survive and keep processing
    std::vector<std::uint8_t> junk(state.size() / 2 + 3, 0xA5);
    (void)plug->loadState(junk);
    std::vector<std::uint8_t> cut(state.begin(), state.begin() + static_cast<std::ptrdiff_t>(state.size() / 2));
    (void)plug->loadState(cut);
    g.render(out);
    for (float v : out.data) CHECK(std::isfinite(v));
    CHECK(plug->loadState(state));                                     // and a good state still loads afterwards
  }

  std::printf(failures ? "FAILED\n" : "vst3 tests passed\n");
  return failures;
}
