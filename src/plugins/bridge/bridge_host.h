#pragma once
#include <memory>
#include <string>
#include <vector>

#include "plugins/plugin_host.h"

namespace wezt::plugins {

// Runs a plugin in a separate helper process (wezt-bridge) and presents it as a normal
// processor. Use it for 32-bit plugins inside 64-bit Wezt, and to isolate unstable plugins.
//
// Threading: process() is for the audio thread. prepare(), saveState(), loadState() and
// parameters() block on the helper and must not run while process() is running.
// The first process() after prepare() already returns the plugin's output for that block:
// the hand-off is synchronous, so the bridge adds no latency of its own.
class BridgedPluginInstance : public IPluginInstance {
 public:
  BridgedPluginInstance(BridgeConfig config, std::string pluginPath, unsigned channels = 2);
  ~BridgedPluginInstance() override;

  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;
  std::size_t latencySamples() const override;

  std::size_t parameterCount() const override;
  std::vector<std::uint8_t> saveState() const override;
  bool loadState(const std::vector<std::uint8_t>& state) override;

  std::vector<ParamInfo> parameters() const override;
  // Lock-free; applied at next block. A bridged plugin cannot be asked for its current value, so
  // parameterNormalized() returns the last value set through Wezt (or the default).
  bool setParameterNormalized(unsigned id, double value) override;
  bool automateParameter(unsigned id, double value) override;
  bool supportsAutomation() const override { return true; }
  double parameterNormalized(unsigned id) const override;

  bool ready() const;
  bool crashed() const;               // helper died or stopped answering
  unsigned droppedBlocks() const;     // blocks replaced by silence
  std::string pluginName() const;
  std::string lastError() const;
  void killHelperForTesting();          // kills the helper and releases the mapping
  void killHelperProcessForTesting();   // kills only the helper process, as a real plugin crash would
  long helperPidForTesting() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wezt::plugins
