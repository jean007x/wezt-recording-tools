#pragma once
// CLAP hosting (https://github.com/free-audio/clap, MIT). Only compiled when WEZT_ENABLE_CLAP is ON.
// CLAP is a good "universal" format: an open C ABI, no vendor SDK or licence, and most major
// plugin vendors ship it next to VST3. Keep CLAP headers out of this header.
#include <memory>
#include <string>
#include <vector>

#include "plugins/plugin_host.h"

namespace wezt::plugins {

struct ClapClassInfo {
  std::string id, name, vendor, version;
};

// Lists the plugins inside a .clap file without instantiating any of them.
std::vector<ClapClassInfo> describeClap(const std::string& path, std::string* error);

class ClapInstance : public IPluginInstance {
 public:
  // `pluginId` empty = the first plugin in the file.
  static std::unique_ptr<ClapInstance> load(const std::string& path, std::string* error, const std::string& pluginId = {});
  ~ClapInstance() override;

  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;  // audio thread
  std::size_t latencySamples() const override;

  // MIDI: true when the plug-in has a note input (CLAP note events, or MIDI bytes). Events arrive on the
  // audio thread just before process() and are delivered at their sample offsets.
  bool acceptsMidi() const override;
  void handleMidi(const MidiEvent* events, std::size_t count) override;

  std::size_t parameterCount() const override;
  std::vector<std::uint8_t> saveState() const override;
  bool loadState(const std::vector<std::uint8_t>& state) override;

  std::string name() const;
  std::string displayName() const override { return name(); }
  std::vector<ParamInfo> parameters() const override;
  double parameterNormalized(unsigned id) const override;
  // Lock-free (single producer); applied at the start of the next block. 0..1 is mapped onto the
  // parameter's own min..max.
  bool setParameterNormalized(unsigned id, double normalized) override;
  bool automateParameter(unsigned id, double normalized) override;
  bool supportsAutomation() const override { return true; }
  std::string parameterText(unsigned id, double normalized) const override;
  // The plugin's AI tools through the "wezt.agent/1" extension (include/wezt_agent.h); empty when it has none.
  std::vector<AiToolInfo> aiTools() const override;
  std::string callAiTool(const std::string& name, const std::string& argsJson, bool& ok) override;

 private:
  ClapInstance();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wezt::plugins
