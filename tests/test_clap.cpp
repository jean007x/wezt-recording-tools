// Loads a real CLAP plugin binary (the test gain plugin) and runs it inside a Wezt graph.
#include <cmath>
#include <cstdio>
#include <memory>

#include "engine/engine.h"
#include "engine/graph.h"
#include "plugins/clap_host.h"

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
    std::printf("usage: wezt_clap_test <plugin.clap>  (skipped)\n");
    return 0;
  }
  std::string err;

  // Not a plugin / missing file: clear errors, no crash.
  CHECK(!ClapInstance::load("/definitely/not/here.clap", &err));
  CHECK(!err.empty());

  auto classes = describeClap(argv[1], &err);
  CHECK(classes.size() == 1 && classes[0].id == "com.wezt.test.gain" && classes[0].name == "Wezt Test Gain");

  auto inst = ClapInstance::load(argv[1], &err);
  if (!inst) { std::printf("load failed: %s\n", err.c_str()); return 1; }
  CHECK(inst->name() == "Wezt Test Gain");
  CHECK(inst->parameterCount() == 1);
  auto params = inst->parameters();
  CHECK(params.size() == 1 && params[0].name == "Gain" && std::fabs(params[0].defaultNormalized - 1.0) < 1e-9);
  const unsigned gainId = params[0].id;

  // ---- universal control surface and the wezt.agent/1 extension, through the base interface ----
  {
    IPluginInstance& base = *inst;
    CHECK(base.displayName() == "Wezt Test Gain");
    auto bp = base.parameters();
    CHECK(bp.size() == 1 && bp[0].min == 0.0 && bp[0].max == 1.0 && bp[0].steps == 0);
    CHECK(base.parameterText(gainId, 0.5) == "0.500");  // the plugin's own text, not ours
    auto tools = base.aiTools();
    CHECK(tools.size() == 3);
    CHECK(tools.size() == 3 && tools[0].name == "set_gain_db" && tools[0].description.find("decibels") != std::string::npos);
    CHECK(tools.size() == 3 && tools[0].inputSchemaJson.find("gain_db") != std::string::npos);
    bool ok = false;
    std::string r = base.callAiTool("set_gain_db", "{\"gain_db\": -6}", ok);
    CHECK(ok && r.find("-6") != std::string::npos);
    CHECK(std::fabs(base.parameterNormalized(gainId) - 0.501187) < 1e-3);  // the call really changed the plugin
    r = base.callAiTool("set_gain_db", "{}", ok);
    CHECK(!ok && r.find("required") != std::string::npos);                  // failure and its reason come back
    r = base.callAiTool("big_report", "{}", ok);
    CHECK(ok && r.size() == 40000);                                         // answer larger than the first buffer
    r = base.callAiTool("always_fails", "{}", ok);
    CHECK(!ok && !r.empty());
    base.callAiTool("set_gain_db", "{\"gain_db\": 0}", ok);               // back to unity for the audio checks below
  }

  ClapInstance* plug = inst.get();
  Graph g;
  auto src = g.add(std::make_unique<Sine>(1000.0));
  auto fx = g.add(std::move(inst));
  CHECK(g.connect(src, fx));
  g.setOutput(fx);
  Engine engine(g, 256, 2, 48000.0, 5);  // prepares the graph, which activates the plugin
  AudioBuffer out;

  g.render(out);
  CHECK(peakOf(out) > 0.99f && peakOf(out) < 1.001f);  // default gain 1.0

  CHECK(plug->setParameterNormalized(gainId, 0.5));
  g.render(out);
  CHECK(std::fabs(peakOf(out) - 0.5f) < 0.01f);
  CHECK(std::fabs(plug->parameterNormalized(gainId) - 0.5) < 1e-6);

  CHECK(plug->setParameterNormalized(gainId, 0.0));
  g.render(out);
  CHECK(peakOf(out) < 1e-6f);

  // The audio-thread entry used by automation: same effect, and it never waits.
  CHECK(plug->automateParameter(gainId, 0.4));
  g.render(out);
  CHECK(std::fabs(peakOf(out) - 0.4f) < 0.01f);
  CHECK(!plug->automateParameter(gainId + 12345, 0.4));   // unknown parameter

  // State round trip: save at 0.25, change, restore.
  plug->setParameterNormalized(gainId, 0.25);
  g.render(out);
  auto state = plug->saveState();
  CHECK(state.size() == sizeof(double));
  plug->setParameterNormalized(gainId, 1.0);
  g.render(out);
  CHECK(peakOf(out) > 0.99f);
  CHECK(plug->loadState(state));
  g.render(out);
  CHECK(std::fabs(peakOf(out) - 0.25f) < 0.01f);
  CHECK(!plug->loadState({}));
  CHECK(plug->latencySamples() == 0);

  std::printf(failures ? "FAILED\n" : "clap tests passed\n");
  return failures;
}
