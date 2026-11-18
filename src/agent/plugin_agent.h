#pragma once
// The agent-facing control layer for plugins. One service, used by the MCP server (so Claude or any
// other MCP-capable agent can drive plugins), by the app's built-in assistant ("make the bass darker"),
// and by scripts. It works on whatever the Session already holds: it does not keep a second plugin
// registry, and it reaches every plugin only through IPluginInstance, so built-in instruments and
// effects, VST3, CLAP and bridged plugins are all controlled the same way.
//
// What it gives an agent:
//   list / describe   what plugins are on which tracks, and every parameter with its value, unit, range
//                     and meaning (a semantic tag, from the plugin or guessed from its name)
//   set / set many    absolute or relative values, with units ("-6 dB", "2 kHz", "30%", "max"), clamped to range
//   tune              intent -> parameter moves ("brighter", "tighter", "more reverb")
//   command           the same in plain sentences ("make the bass track a little darker")
//   add_plugin        built-in plugins only (never loads a file an agent names)
//   ai tools          what a plugin's own AI can do (CLAP wezt.agent/1), and a call-through
//   history / undo    every change an agent made, reversible
//
// Safety: everything an agent sends is validated (names, ranges, sizes, finite numbers); values are
// clamped to the parameter's range; nothing here opens a file or runs a program; output from a
// plugin's own AI is returned marked as untrusted plugin output.
// Threading: control thread only (the same one that owns the Session); parameters reach the audio
// thread through each plugin's own lock-free hand-off.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "edit/undo.h"
#include "plugins/plugin_host.h"
#include "session/session.h"
#include "util/json.h"

namespace wezt::agent {

struct Result {
  bool ok = true;
  std::string message;                         // for a person; also what a chat agent reads first
  json::Value data = json::Value::object();    // for programs
};

class PluginAgent {
 public:
  // `undo` (optional): changes are also recorded there, one step per agent action, so Ctrl-Z in the app works.
  explicit PluginAgent(session::Session& s, edit::UndoStack* undo = nullptr);
  ~PluginAgent();

  // Structural edits (adding a plugin) must happen while the audio device is stopped; a host that runs audio
  // supplies a wrapper that stops and restarts it. Without one the edit is made directly.
  void setStructuralRunner(std::function<void(const std::function<void()>&)> runner) { structural_ = std::move(runner); }

  Result listPlugins() const;
  Result describe(const std::string& track, const std::string& plugin = {}) const;
  // `value`: a number (in the parameter's own unit), a string with a unit, or "min" / "max" / "default" / "norm:0.4".
  // `relative`: add the value to the current one instead of replacing it.
  Result setParam(const std::string& track, const std::string& plugin, const std::string& param, const json::Value& value, bool relative = false);
  Result setParams(const std::string& track, const std::string& plugin, const json::Value& settings);  // {"cutoff": 800, "release": "0.4 s"}
  Result resetParam(const std::string& track, const std::string& plugin, const std::string& param = {});  // empty param = all
  // Automation: points over time while the song plays. `points`: an array of {"time": seconds, "value": <as in setParam>}
  // (at most 256; times 0 to 24 h; values are clamped to the parameter's range). An empty array removes the lane.
  // Needs a plugin that supports automation (all built-in instruments and effects, VST3 and CLAP plugins).
  Result automate(const std::string& track, const std::string& plugin, const std::string& param, const json::Value& points);
  Result tune(const std::string& track, const std::string& plugin, const std::string& intent);
  Result command(const std::string& text);
  Result addPlugin(const std::string& track, const std::string& builtinName, int position = -1);
  Result aiTools(const std::string& track, const std::string& plugin) const;
  Result callTool(const std::string& track, const std::string& plugin, const std::string& tool, const json::Value& arguments);
  Result history() const;
  Result undoChanges(int batches = 1);

  // Built-in plugins an agent may add.
  static std::vector<std::string> addablePlugins();

 private:
  struct Target;
  struct Entry {
    Entry(plugins::IPluginInstance* i, unsigned pid, std::string name, double b, double a) : inst(i), id(pid), param(std::move(name)), before(b), after(a) {}
    plugins::IPluginInstance* inst; unsigned id; std::string param; double before, after;
    bool isLane = false; session::TrackId track = -1; std::size_t slot = 0;   // an automation lane edit: the lane's points before and after
    std::vector<session::AutomationPoint> beforePts, afterPts;
  };
  struct Batch { int id; std::string label, track, plugin; std::vector<Entry> entries; bool undone = false; };

  bool resolveTrack(const std::string& name, session::TrackId& out, std::string& err) const;
  bool resolvePlugin(session::TrackId t, const std::string& selector, Target& out, std::string& err) const;
  bool resolveParam(session::TrackId t, const std::string& pluginSel, const std::string& param, Target& out, plugins::ParamInfo& pi, std::string& err) const;
  bool alive(const plugins::IPluginInstance* p) const;
  void restore(const std::vector<Entry>& entries, bool toBefore);
  int commit(const Target& tg, const std::string& label, std::vector<Entry> entries);

  session::Session& s_;
  edit::UndoStack* undo_;
  std::function<void(const std::function<void()>&)> structural_;
  std::vector<Batch> history_;
  int nextBatch_ = 1;
  std::shared_ptr<int> token_;   // lets undo steps in the app's stack notice the agent is gone
};

}  // namespace wezt::agent
