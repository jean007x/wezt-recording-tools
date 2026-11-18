// The universal plugin-control interface for agents: the built-in effects as plugins, the agent service
// (list, describe, set, tune, command, undo, plugin AI tools), and the MCP tools on top of it.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "agent/plugin_agent.h"
#include "agent/semantics.h"
#include "dsp/effect_plugins.h"
#include "instruments/builtin.h"
#include "mcp/server.h"
#include "session/session.h"

using namespace wezt;
using json::Value;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
#define CHECK_MSG(c, m) do { if (!(c)) { std::printf("FAIL line %d: %s  (%s)\n", __LINE__, #c, std::string(m).c_str()); ++fails; } } while (0)

namespace {
constexpr double kSr = 48000.0;

// RMS of one steady sine of `hz` through a processor (after a settling period), in dB re full scale.
double toneDb(Processor& p, double hz, double ampDb, std::size_t frames = 24000) {
  AudioBuffer in(2, frames), out(2, frames);
  const double a = std::pow(10.0, ampDb / 20.0);
  for (std::size_t i = 0; i < frames; ++i) in.channel(0)[i] = in.channel(1)[i] = static_cast<float>(a * std::sin(2 * M_PI * hz * static_cast<double>(i) / kSr));
  p.prepare(kSr, frames);
  p.process(in, out);
  double s = 0; std::size_t n = 0;
  for (std::size_t i = frames / 2; i < frames; ++i, ++n) s += static_cast<double>(out.channel(0)[i]) * out.channel(0)[i];
  return 10.0 * std::log10(s / static_cast<double>(n) + 1e-20);
}
void setPlain(plugins::IPluginInstance& p, unsigned id, double plain) {
  const auto ps = p.parameters();
  const auto& i = ps[id];
  double n = (i.logScale && i.min > 0) ? std::log(plain / i.min) / std::log(i.max / i.min) : (plain - i.min) / (i.max - i.min);
  p.setParameterNormalized(id, n);
}

// A plugin with AI tools of its own, standing in for a third-party AI plugin.
class FakeAi : public plugins::IPluginInstance {
 public:
  std::size_t parameterCount() const override { return 1; }
  std::vector<std::uint8_t> saveState() const override { return {}; }
  bool loadState(const std::vector<std::uint8_t>&) override { return true; }
  void process(const AudioBuffer& in, AudioBuffer& out) override { for (std::size_t c = 0; c < std::min(in.channels, out.channels); ++c) std::copy(in.channel(c), in.channel(c) + out.frames, out.channel(c)); }
  std::string displayName() const override { return "Fake AI Mastering"; }
  std::vector<plugins::ParamInfo> parameters() const override { plugins::ParamInfo p; p.id = 0; p.name = "Amount"; p.semantic = "depth"; return {p}; }
  double parameterNormalized(unsigned) const override { return amount_; }
  bool setParameterNormalized(unsigned, double n) override { amount_ = n; return true; }
  std::vector<plugins::AiToolInfo> aiTools() const override { return {{"echo", "Returns its arguments", "{\"type\":\"object\"}"}, {"big", "Returns a huge answer", "{}"}, {"boom", "Always fails", "{}"}}; }
  std::string callAiTool(const std::string& name, const std::string& args, bool& ok) override {
    ok = true; lastArgs = args;
    if (name == "echo") return "echo:" + args;
    if (name == "big") return std::string(200000, 'x');
    ok = false; return "it broke";
  }
  std::string lastArgs;
 private:
  double amount_ = 0;
};
}  // namespace

int main() {
  // ---------------------------------------------------------------- the built-in effects are plugins
  {
    dsp::EqPlugin eq;
    CHECK(eq.parameterCount() == 10 && eq.displayName() == "Wezt EQ");
    const double flat = toneDb(eq, 15000, -12);                 // well above the shelf's corner (a shelf gives only half its gain at the corner)
    setPlain(eq, dsp::EqPlugin::HighGain, 12);
    const double boosted = toneDb(eq, 15000, -12);
    CHECK_MSG(boosted > flat + 9 && boosted < flat + 14, std::to_string(boosted - flat));
    dsp::EqPlugin eq2;
    setPlain(eq2, dsp::EqPlugin::LowGain, -12);
    CHECK(toneDb(eq2, 60, -12) < toneDb(eq, 60, -12) - 6);

    dsp::CompressorPlugin comp;
    const double open = toneDb(comp, 1000, -6);
    setPlain(comp, dsp::CompressorPlugin::Threshold, -30);
    setPlain(comp, dsp::CompressorPlugin::Ratio, 10);
    setPlain(comp, dsp::CompressorPlugin::Attack, 1);
    const double squashed = toneDb(comp, 1000, -6);
    CHECK_MSG(squashed < open - 8, std::to_string(open - squashed));

    dsp::LimiterPlugin lim;
    setPlain(lim, dsp::LimiterPlugin::Ceiling, -6);
    AudioBuffer in(2, 24000), out(2, 24000);
    for (std::size_t i = 0; i < 24000; ++i) in.channel(0)[i] = in.channel(1)[i] = static_cast<float>(0.95 * std::sin(2 * M_PI * 200.0 * static_cast<double>(i) / kSr));
    lim.prepare(kSr, 24000); lim.process(in, out);
    float pk = 0; for (std::size_t i = 12000; i < 24000; ++i) pk = std::max(pk, std::fabs(out.channel(0)[i]));
    CHECK_MSG(pk <= 0.5013f, std::to_string(pk));
    CHECK(lim.latencySamples() > 0);

    dsp::WidthPlugin w;
    setPlain(w, 0, 0);
    AudioBuffer a(2, 256), b(2, 256);
    for (std::size_t i = 0; i < 256; ++i) { a.channel(0)[i] = 0.5f; a.channel(1)[i] = -0.25f; }
    w.prepare(kSr, 256); w.process(a, b);
    CHECK(std::fabs(b.channel(0)[10] - b.channel(1)[10]) < 1e-6f);

    // state round trip
    dsp::EqPlugin e3; setPlain(e3, dsp::EqPlugin::HighGain, 7);
    dsp::EqPlugin e4; CHECK(e4.loadState(e3.saveState()));
    CHECK(std::fabs(e4.parameterNormalized(dsp::EqPlugin::HighGain) - e3.parameterNormalized(dsp::EqPlugin::HighGain)) < 1e-6);
    CHECK(!e4.setParameterNormalized(99, 0.5) && !e4.setParameterNormalized(0, std::numeric_limits<double>::quiet_NaN()));
    // the factory builds them from descriptors (session load / duplicate)
    for (auto& [name, inst] : instruments::builtinPlugins()) { plugins::PluginDescriptor d = dsp::EqPlugin::descriptor(); d.name = name; auto p = instruments::makeBuiltin(d); CHECK(p != nullptr); (void)inst; }
  }

  // ---------------------------------------------------------------- semantics
  {
    CHECK(agent::guessSemantic("Filter Cutoff", "Hz") == "cutoff");
    CHECK(agent::guessSemantic("Dry/Wet", "%") == "mix");
    CHECK(agent::guessSemantic("Quality", "") != "resonance");   // "q" is a whole word, not a letter inside another
    CHECK(agent::guessSemantic("Zorp", "") == "");
    const auto i = agent::parseIntent("make it a little brighter");
    CHECK(!i.moves.empty() && i.amount < 0.1);
    CHECK(agent::parseIntent("much more reverb").amount > 0.2);
    CHECK(agent::parseIntent("lower the cutoff").moves.size() == 1 && agent::parseIntent("lower the cutoff").moves[0].dir == -1);
    CHECK(agent::parseIntent("blorp the wibble").moves.empty());
    CHECK(agent::tagMatches("attack", "comp_attack") && !agent::tagMatches("=attack", "comp_attack") && agent::tagMatches("=attack", "attack"));
  }

  // ---------------------------------------------------------------- the agent service on a session
  session::Session s(kSr, 2, 256);
  const auto bass = s.addTrack("Bass", false, {});
  const auto drums = s.addTrack("Drums", false, {});
  (void)drums;
  edit::UndoStack undo;
  agent::PluginAgent ag(s, &undo);

  CHECK(ag.listPlugins().message.find("no plugins") != std::string::npos);
  CHECK(ag.addPlugin("bass", "eq").ok);
  CHECK(ag.addPlugin("bass", "Wezt Compressor").ok);
  CHECK(ag.addPlugin("bass", "synth").ok);
  CHECK(ag.addPlugin("drums", "limiter").ok);
  CHECK(!ag.addPlugin("bass", "/tmp/evil.vst3").ok && !ag.addPlugin("bass", "../../x.clap").ok);   // only Wezt's own plugins
  CHECK(!ag.addPlugin("nope", "eq").ok);
  CHECK(s.track(bass)->inserts.size() == 3);
  CHECK(s.track(bass)->inserts[0].instance->acceptsMidi());                 // instruments go first
  CHECK(s.track(bass)->inserts[2].instance->displayName() == "Wezt Compressor");

  {
    auto r = ag.listPlugins();
    CHECK(r.ok && r.data.get("plugins")->items().size() == 4);
    CHECK(r.message.find("instrument") != std::string::npos && r.message.find("effect") != std::string::npos);
  }
  {
    auto r = ag.describe("bass", "eq");
    CHECK(r.ok && r.data.get("parameter_count")->num() == 10);
    CHECK(r.message.find("High gain = 0.0 dB") != std::string::npos || r.message.find("High gain = 0.00 dB") != std::string::npos);
    CHECK(!ag.describe("bass", "").ok);                                      // three plugins: it says which
    CHECK(ag.describe("bass", "").message.find("Plugins:") != std::string::npos);
    CHECK(!ag.describe("bass", "reverb").ok);
    CHECK(ag.describe("bass", "2").ok);                                       // by slot
    CHECK(ag.describe("drums", "").ok);                                       // single plugin: implied
  }

  const auto eqPlugin = [&]() { return s.track(bass)->inserts[1].instance; };
  {
    // loose names, units, clamping
    auto r = ag.setParam("bass", "eq", "high gain", Value::string("6 dB"));
    CHECK_MSG(r.ok, r.message);
    CHECK(std::fabs(eqPlugin()->parameterNormalized(dsp::EqPlugin::HighGain) - 21.0 / 30.0) < 1e-6);
    CHECK(ag.setParam("bass", "eq", "treble", Value::number(-3)).ok);       // by meaning
    CHECK(ag.setParam("bass", "eq", "high gain", Value::number(3), true).ok);   // relative: -3 + 3 = 0
    CHECK(std::fabs(eqPlugin()->parameterNormalized(dsp::EqPlugin::HighGain) - 0.5) < 1e-6);
    r = ag.setParam("bass", "eq", "high freq", Value::string("5 kHz"));
    CHECK_MSG(r.ok, r.message);
    r = ag.setParam("bass", "eq", "high gain", Value::number(100));
    CHECK(r.ok && r.data.getBool("clamped") && std::fabs(eqPlugin()->parameterNormalized(dsp::EqPlugin::HighGain) - 1.0) < 1e-9);
    CHECK(!ag.setParam("bass", "eq", "high gain", Value::string("5 kHz")).ok);           // wrong unit
    CHECK(!ag.setParam("bass", "eq", "high gain", Value::number(std::numeric_limits<double>::infinity())).ok);
    CHECK(!ag.setParam("bass", "eq", "high gain", Value::string("1e999 dB")).ok);
    CHECK(!ag.setParam("bass", "eq", "high gain", Value::string("banana")).ok);
    CHECK(!ag.setParam("bass", "eq", "high gain", Value::array()).ok);
    r = ag.setParam("bass", "eq", "gain", Value::number(1));
    CHECK(!r.ok && r.message.find("could mean") != std::string::npos);                    // ambiguous: it asks, it does not guess
    r = ag.setParam("bass", "eq", "zzz", Value::number(1));
    CHECK(!r.ok && r.message.find("It has:") != std::string::npos);
    CHECK(ag.setParam("bass", "eq", "id:1", Value::string("max")).ok && ag.setParam("bass", "eq", "#1", Value::string("default")).ok);
    CHECK(!ag.setParam("bass", "eq", "id:99", Value::number(1)).ok);
    // no plugin named: the one plugin on the track that has that parameter
    CHECK(ag.setParam("bass", "", "threshold", Value::string("-24 dB")).ok);
    CHECK(!ag.setParam("bass", "", "attack", Value::number(1)).ok);                        // synth and compressor both have one
    CHECK(ag.setParam("bass", "synth", "cutoff", Value::string("1.5 kHz")).ok);
    CHECK(ag.setParam("bass", "compressor", "attack", Value::string("0.005 s")).ok);       // s -> ms
    CHECK(std::fabs(s.track(bass)->inserts[2].instance->parameterText(2, s.track(bass)->inserts[2].instance->parameterNormalized(2)).size() > 0 ? 1.0 : 0.0) == 1.0);
  }
  {
    // several at once: all checked first
    Value st = Value::object(); st.set("high gain", Value::number(4)).set("low gain", Value::string("-2 dB"));
    CHECK(ag.setParams("bass", "eq", st).ok);
    Value bad = Value::object(); bad.set("high gain", Value::number(9)).set("nonsense", Value::number(1));
    const double hg = eqPlugin()->parameterNormalized(dsp::EqPlugin::HighGain);
    CHECK(!ag.setParams("bass", "eq", bad).ok && eqPlugin()->parameterNormalized(dsp::EqPlugin::HighGain) == hg);   // nothing changed
    Value big = Value::object(); for (int i = 0; i < 70; ++i) big.set("p" + std::to_string(i), Value::number(1));
    CHECK(!ag.setParams("bass", "eq", big).ok);
    CHECK(!ag.setParams("bass", "eq", Value::object()).ok);
  }
  {
    // automation over time, by units; bad input changes nothing; undo restores the lane
    auto pt = [](double t, const Value& v) { Value o = Value::object(); o.set("time", Value::number(t)).set("value", v); return o; };
    Value pts = Value::array();
    pts.push(pt(8, Value::string("12 dB"))); pts.push(pt(0, Value::number(0)));   // out of order on purpose
    auto r = ag.automate("bass", "eq", "high gain", pts);
    CHECK_MSG(r.ok, r.message);
    const auto* lane = s.paramAutomation(bass, 1, dsp::EqPlugin::HighGain);
    CHECK(lane && lane->points().size() == 2);
    if (lane) { CHECK(lane->points()[0].frame == 0 && std::fabs(lane->points()[0].value - 0.5) < 1e-6); CHECK(lane->points()[1].frame == 8 * 48000 && std::fabs(lane->points()[1].value - 0.9) < 1e-6); }
    Value bad = Value::array(); bad.push(pt(-1, Value::number(0)));
    CHECK(!ag.automate("bass", "eq", "high gain", bad).ok);                                      // negative time
    bad = Value::array(); bad.push(pt(1, Value::number(0))); bad.push(pt(1, Value::number(3)));
    CHECK(!ag.automate("bass", "eq", "high gain", bad).ok);                                      // same time twice
    bad = Value::array(); for (int i = 0; i < 300; ++i) bad.push(pt(i, Value::number(0)));
    CHECK(!ag.automate("bass", "eq", "high gain", bad).ok);                                      // too many points
    bad = Value::array(); bad.push(pt(1, Value::string("banana")));
    CHECK(!ag.automate("bass", "eq", "high gain", bad).ok);
    CHECK(!ag.automate("bass", "eq", "no such parameter", pts).ok && !ag.automate("nope", "eq", "high gain", pts).ok && !ag.automate("bass", "eq", "high gain", Value::string("x")).ok);
    CHECK(s.paramAutomation(bass, 1, dsp::EqPlugin::HighGain)->points().size() == 2);            // all the failures changed nothing
    Value more = Value::array(); more.push(pt(0, Value::number(-6))); more.push(pt(2, Value::number(6)));
    CHECK(ag.automate("bass", "eq", "high gain", more).ok && s.paramAutomation(bass, 1, dsp::EqPlugin::HighGain)->points().size() == 2);
    CHECK(ag.undoChanges(1).ok);                                                                  // back to the first version
    CHECK(std::fabs(s.paramAutomation(bass, 1, dsp::EqPlugin::HighGain)->points()[1].value - 0.9) < 1e-6);
    CHECK(ag.automate("bass", "eq", "high gain", Value::array()).ok && s.paramAutomation(bass, 1, dsp::EqPlugin::HighGain) == nullptr);   // empty list removes it
    CHECK(undo.canUndo() && undo.undo() && s.paramAutomation(bass, 1, dsp::EqPlugin::HighGain) != nullptr);                                  // and the app's undo brings it back
    CHECK(ag.history().message.find("automate") != std::string::npos);
    s.setParamAutomation(bass, 1, dsp::EqPlugin::HighGain, {});
  }
  {
    // history and undo (the agent's own, and the app's undo stack)
    const double before = eqPlugin()->parameterNormalized(dsp::EqPlugin::LowGain);
    ag.setParam("bass", "eq", "low gain", Value::number(9));
    CHECK(eqPlugin()->parameterNormalized(dsp::EqPlugin::LowGain) != before);
    CHECK(ag.history().message.find("#") != std::string::npos);
    CHECK(ag.undoChanges(1).ok && std::fabs(eqPlugin()->parameterNormalized(dsp::EqPlugin::LowGain) - before) < 1e-9);
    ag.setParam("bass", "eq", "low gain", Value::number(9));
    CHECK(undo.canUndo() && undo.undo() && std::fabs(eqPlugin()->parameterNormalized(dsp::EqPlugin::LowGain) - before) < 1e-9);
    CHECK(undo.redo() && eqPlugin()->parameterNormalized(dsp::EqPlugin::LowGain) != before);
    CHECK(ag.resetParam("bass", "eq", "low gain").ok && std::fabs(eqPlugin()->parameterNormalized(dsp::EqPlugin::LowGain) - 0.5) < 1e-9);
    CHECK(ag.resetParam("bass", "eq").ok);
    CHECK(ag.resetParam("bass", "eq").message.find("already at its defaults") != std::string::npos);
  }

  // ---------------------------------------------------------------- tuning by intent
  {
    ag.resetParam("bass", "eq"); ag.resetParam("bass", "synth"); ag.resetParam("bass", "compressor");
    auto hg = [&] { return eqPlugin()->parameterNormalized(dsp::EqPlugin::HighGain); };
    auto r = ag.tune("bass", "eq", "a little brighter");
    CHECK_MSG(r.ok, r.message);
    CHECK(hg() > 0.5 && hg() < 0.6);
    const double afterOnce = hg();
    CHECK(ag.tune("bass", "eq", "a lot darker").ok && hg() < afterOnce);
    // the whole track: "darker" lowers the EQ's treble and the synth's cutoff together
    ag.resetParam("bass", "eq"); ag.resetParam("bass", "synth");
    const auto synthCut = s.track(bass)->inserts[0].instance->parameterNormalized(7);
    r = ag.tune("bass", "", "darker");
    CHECK(r.ok && hg() < 0.5 && s.track(bass)->inserts[0].instance->parameterNormalized(7) < synthCut);
    // punch: the compressor attack goes up, a synth attack goes down
    ag.resetParam("bass", "synth"); ag.resetParam("bass", "compressor");
    const double catk = s.track(bass)->inserts[2].instance->parameterNormalized(dsp::CompressorPlugin::Attack), satk = s.track(bass)->inserts[0].instance->parameterNormalized(9);
    CHECK(ag.tune("bass", "", "punchier").ok);
    CHECK(s.track(bass)->inserts[2].instance->parameterNormalized(dsp::CompressorPlugin::Attack) > catk && s.track(bass)->inserts[0].instance->parameterNormalized(9) < satk);
    // contradictory words cancel; unknown words are explained, never guessed
    const double h0 = hg();
    r = ag.tune("bass", "eq", "brighter and darker");
    CHECK(hg() == h0);
    r = ag.tune("bass", "eq", "more wibble");
    CHECK(!r.ok && r.message.find("did not understand") != std::string::npos);
    r = ag.tune("bass", "eq", "more reverb");   // understood, but an EQ has no mix control
    CHECK(!r.ok && r.message.find("Nothing on") != std::string::npos && r.message.find("treble") != std::string::npos);
    CHECK(!ag.tune("bass", "eq", "").ok);
    // limits: pushing past the end reports it
    for (int i = 0; i < 12; ++i) ag.tune("bass", "eq", "a lot brighter");
    CHECK(ag.tune("bass", "eq", "a lot brighter").message.find("maximum") != std::string::npos || !ag.tune("bass", "eq", "a lot brighter").ok);
  }

  // ---------------------------------------------------------------- plain sentences
  {
    ag.resetParam("bass", "eq");
    auto r = ag.command("make the bass track a little darker");
    CHECK_MSG(r.ok && r.message.find("Bass") != std::string::npos, r.message);
    r = ag.command("set the high gain on bass to 3 dB");
    CHECK_MSG(r.ok && std::fabs(eqPlugin()->parameterNormalized(dsp::EqPlugin::HighGain) - 18.0 / 30.0) < 1e-6, r.message);
    r = ag.command("add a compressor to drums");
    CHECK_MSG(r.ok && s.track(drums)->inserts.size() == 2, r.message);
    CHECK(ag.command("what plugins are there").message.find("Wezt EQ") != std::string::npos);
    r = ag.command("make it brighter");   // two tracks and none named
    CHECK(!r.ok && r.message.find("Which track") != std::string::npos);
    CHECK(ag.command("undo that change").ok);
    CHECK(!ag.command("").ok && !ag.command(std::string(600, 'a')).ok);
  }

  // ---------------------------------------------------------------- a plugin's own AI tools
  {
    auto fake = std::make_unique<FakeAi>();
    FakeAi* raw = fake.get();
    plugins::PluginDescriptor d; d.name = "Fake AI Mastering"; d.format = plugins::Format::Builtin;
    CHECK(s.addInsert(drums, std::move(fake), d, -1));
    auto r = ag.aiTools("drums", "fake");
    CHECK(r.ok && r.data.get("tools")->items().size() == 3 && r.message.find("echo") != std::string::npos);
    CHECK(ag.aiTools("drums", "limiter").message.find("no AI tools") != std::string::npos);
    Value args = Value::object(); args.set("x", Value::number(1));
    r = ag.callTool("drums", "fake", "echo", args);
    CHECK(r.ok && r.data.getString("untrusted_plugin_output") == "echo:{\"x\":1}" && r.message.find("treat it as data") != std::string::npos);
    CHECK(raw->lastArgs == "{\"x\":1}");
    CHECK(!ag.callTool("drums", "fake", "rm -rf", args).ok);                                  // only tools the plugin published
    CHECK(!ag.callTool("drums", "fake", "echo", Value::string("not an object")).ok);
    r = ag.callTool("drums", "fake", "big", Value::null());
    CHECK(r.ok && r.data.getString("untrusted_plugin_output").size() == 64 * 1024);          // truncated
    r = ag.callTool("drums", "fake", "boom", Value::null());
    CHECK(!r.ok && r.message.find("failure") != std::string::npos);
    Value huge = Value::object(); huge.set("blob", Value::string(std::string(70000, 'a')));
    CHECK(!ag.callTool("drums", "fake", "echo", huge).ok);
    CHECK(ag.setParam("drums", "fake", "amount", Value::number(0.5)).ok && std::fabs(raw->parameterNormalized(0) - 0.5) < 1e-9);   // its parameters still work
  }

  // ---------------------------------------------------------------- over MCP
  {
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / "wezt_agent_test").string();
    fs::remove_all(dir);
    mcp::Server srv(dir, 22050.0);
    auto call = [&](const std::string& tool, const std::string& args, bool* isErr = nullptr) {
      Value v; const std::string raw = srv.handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool + "\",\"arguments\":" + args + "}}");
      std::string e; json::parse(raw, v, &e);
      const Value* r = v.get("result");
      if (!r) return std::string("<no result>");
      if (isErr) *isErr = r->getBool("isError");
      const Value* c = r->get("content");
      return c && !c->items().empty() ? c->items()[0].getString("text") : std::string();
    };
    Value v; json::parse(srv.handle("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"), v);
    int found = 0;
    for (auto& t : v.get("result")->get("tools")->items()) {
      const std::string n = t.getString("name");
      for (const char* want : {"list_plugins", "describe_plugin", "set_plugin_param", "set_plugin_params", "reset_plugin", "tune_plugin", "control_plugins", "add_plugin", "plugin_ai_tools", "call_plugin_tool", "agent_history", "undo_agent_changes"}) if (n == want) ++found;
    }
    CHECK(found == 12);
    bool err = false;
    call("add_part", "{\"kind\":\"bass\",\"name\":\"Bass\"}", &err); CHECK(!err);
    std::string t = call("list_plugins", "{}", &err); CHECK(!err && t.find("no plugins") != std::string::npos);
    t = call("add_plugin", "{\"track\":\"Bass\",\"plugin\":\"Wezt EQ\"}", &err); CHECK_MSG(!err, t);
    t = call("add_plugin", "{\"track\":\"Bass\",\"plugin\":\"C:\\\\evil.vst3\"}", &err); CHECK(err);
    t = call("describe_plugin", "{\"track\":\"Bass\"}", &err);
    CHECK(!err && t.find("High gain") != std::string::npos && t.find("\"meaning\":\"treble\"") != std::string::npos);
    t = call("set_plugin_param", "{\"track\":\"Bass\",\"parameter\":\"high gain\",\"value\":\"+4 dB\"}", &err);
    CHECK_MSG(!err, t);
    t = call("set_plugin_param", "{\"track\":\"Bass\",\"parameter\":\"high gain\",\"value\":2.5}", &err);
    CHECK_MSG(!err && t.find("2.5") != std::string::npos, t);
    call("set_plugin_param", "{\"track\":\"Bass\",\"parameter\":\"high gain\"}", &err); CHECK(err);                    // value missing
    t = call("tune_plugin", "{\"track\":\"Bass\",\"intent\":\"warmer\"}", &err); CHECK_MSG(!err, t);
    t = call("control_plugins", "{\"command\":\"make the bass thicker\"}", &err); CHECK_MSG(!err, t);
    t = call("set_plugin_params", "{\"track\":\"Bass\",\"settings\":{\"low gain\":3,\"high gain\":-1}}", &err); CHECK_MSG(!err, t);
    t = call("agent_history", "{}", &err); CHECK(!err && t.find("#") != std::string::npos);
    t = call("undo_agent_changes", "{\"count\":2}", &err); CHECK(!err);
    t = call("plugin_ai_tools", "{\"track\":\"Bass\"}", &err); CHECK(!err && t.find("no AI tools") != std::string::npos);
    call("call_plugin_tool", "{\"track\":\"Bass\",\"tool\":\"x\"}", &err); CHECK(err);
    call("reset_plugin", "{\"track\":\"Bass\"}", &err); CHECK(!err);
    call("describe_plugin", "{\"track\":\"Nope\"}", &err); CHECK(err);
  }

  if (fails == 0) std::printf("agent interface tests passed\n");
  return fails == 0 ? 0 : 1;
}
