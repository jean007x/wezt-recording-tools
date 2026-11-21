#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/graph.h"
#include "engine/midi_event.h"

namespace wezt::plugins {

// RTAS and TDM are legacy Avid formats that share the .dpm extension, so a scan cannot tell them
// apart without opening the file (which we never do for Pro Tools-only formats).
enum class Format { VST3, VST2, AU, AAX, CLAP, Bridged32, RTAS, TDM, Builtin };  // Builtin = instruments and effects that ship with Wezt (appended: saved sessions store the number)

struct PluginDescriptor {
  std::string name, vendor, path;
  Format format = Format::VST3;
  bool is32bit = false;
  bool sandbox = false;  // run in the separate wezt-bridge process, so a crash of the plug-in cannot take Wezt down (effects only)
  int bitness = 0;  // 32, 64, or 0 when unknown (read from the file header, never by loading it)
  std::string note;
};

// One automatable parameter, in the order {id, name, default, hidden}. The fields after `hidden`
// are optional extras that make a parameter understandable to an AI agent: a plain-unit range
// (min..max, e.g. 20..20000 Hz), a unit, a step count for switches, and a semantic tag such as
// "cutoff", "gain" or "attack". Hosts for third-party plugins fill what the plugin reports; the
// semantic tag is guessed from the name when the plugin gives none (see agent/semantics.h).
struct ParamInfo {
  unsigned id = 0;
  std::string name;
  double defaultNormalized = 0.0;
  bool hidden = false;
  double min = 0.0, max = 1.0;   // plain range; 0..1 when the plugin only reports normalized values
  std::string unit;              // "dB", "Hz", "ms", "%", "" ...
  std::string group;             // section the plugin puts it in, when it says
  std::string semantic;          // see agent/semantics.h
  int steps = 0;                 // 0 = continuous, N = N+1 discrete positions (1 = on/off switch)
  bool logScale = false;         // normalized <-> plain follows an exponential curve
};

// One thing a plugin's own AI can do, offered to agents: a name, what it does, and a JSON Schema
// for its arguments (as text). See include/wezt_agent.h for how third-party plugins publish these.
struct AiToolInfo {
  std::string name, description, inputSchemaJson;
};

struct IPluginInstance : Processor {
  virtual std::size_t parameterCount() const = 0;
  virtual std::vector<std::uint8_t> saveState() const = 0;
  virtual bool loadState(const std::vector<std::uint8_t>&) = 0;

  // ---- MIDI (instruments) ----
  // True for plugins that make sound from notes. The track's sequencer, and the key preview, call
  // handleMidi() on the AUDIO thread immediately before process() for the same block, with the
  // events' sample offsets inside that block. It must not allocate or block.
  // ---- plugin-initiated changes (control thread) ----
  // True once after the plugin announced that its latency changed (VST3 restartComponent / CLAP latency changed); the
  // caller then runs Session::applyPluginLatency() while the audio is stopped.
  virtual bool takeLatencyChanged() { return false; }
  virtual bool acceptsMidi() const { return false; }
  virtual void handleMidi(const MidiEvent* events, std::size_t count) { (void)events; (void)count; }

  // ---- universal control surface (control thread; the defaults mean "nothing to control") ----
  virtual std::string displayName() const { return {}; }
  virtual std::vector<ParamInfo> parameters() const { return {}; }
  virtual double parameterNormalized(unsigned id) const { (void)id; return 0.0; }
  // Lock-free hand-off to the audio thread; false when the id is unknown or the queue is full.
  virtual bool setParameterNormalized(unsigned id, double normalized) { (void)id; (void)normalized; return false; }
  // The same, for the AUDIO thread (automation): never blocks, allocates or waits. Returns false when the
  // plugin does not support it or was busy this time; the caller tries again next block.
  virtual bool automateParameter(unsigned id, double normalized) { (void)id; (void)normalized; return false; }
  virtual bool supportsAutomation() const { return false; }   // true when automateParameter works
  // The plugin's own text for a value ("-6.0 dB", "2.5 kHz"); empty when it has none.
  virtual std::string parameterText(unsigned id, double normalized) const { (void)id; (void)normalized; return {}; }
  // The plugin's AI capabilities, if it has any, and a way to call them. callAiTool returns the
  // result as text (JSON or prose); `ok` is false when the call failed. Control thread only.
  virtual std::vector<AiToolInfo> aiTools() const { return {}; }
  virtual std::string callAiTool(const std::string& name, const std::string& argsJson, bool& ok) { (void)name; (void)argsJson; ok = false; return "this plugin has no AI tools"; }
};

struct IPluginFormat {
  virtual ~IPluginFormat() = default;
  virtual Format format() const = 0;
  virtual bool supportedOnThisPlatform() const = 0;
  virtual std::vector<PluginDescriptor> scan(const std::vector<std::string>& dirs) = 0;
  virtual std::unique_ptr<IPluginInstance> create(const PluginDescriptor&) = 0;
};

// Out-of-process host for plugins whose bitness differs from the host (e.g. 32-bit on Windows).
// Protocol: shared-memory audio ring + control socket (see docs/PLUGIN_FORMATS.md).
struct BridgeConfig {
  std::string helperExecutable;  // wezt-bridge built with the plugin's bitness (e.g. 32-bit)
  int timeoutMs = 250;           // a block that takes longer is dropped (silence) and counted
  int startupTimeoutMs = 15000;  // plugin load can be slow
  std::vector<std::string> helperArgs;  // extra helper command-line arguments (diagnostics and tests)
};

}  // namespace wezt::plugins
