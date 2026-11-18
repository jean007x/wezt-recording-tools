#pragma once
// VST3 hosting built on Steinberg's open-source VST3 SDK (MIT). Only compiled when
// WEZT_ENABLE_VST3 is ON. Keep SDK headers out of this header so the rest of the engine
// never depends on them.
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "plugins/plugin_host.h"

namespace wezt::plugins {

using Vst3ParamInfo = ParamInfo;

struct Vst3ClassInfo {
  std::string name, vendor, category, subCategories;
};

// Lists audio-module classes inside a .vst3 bundle/file without instantiating them.
std::vector<Vst3ClassInfo> describeVst3(const std::string& modulePath, std::string* error);

class Vst3Instance : public IPluginInstance {
 public:
  // Loads the first "Audio Module Class" found in the module, or the one called `className` when
  // given (a bundle such as mda-vst3 holds many).
  static std::unique_ptr<Vst3Instance> load(const std::string& modulePath, std::string* error, const std::string& className = {});
  ~Vst3Instance() override;

  // Processor interface (audio thread: process only)
  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;
  std::size_t latencySamples() const override;
  bool takeLatencyChanged() override;

  // IPluginInstance
  std::size_t parameterCount() const override;
  std::vector<std::uint8_t> saveState() const override;
  bool loadState(const std::vector<std::uint8_t>& state) override;

  std::string name() const;
  std::string displayName() const override { return name(); }
  std::vector<Vst3ParamInfo> parameters() const override;
  double parameterNormalized(unsigned id) const override;
  // Thread-safe, lock-free (single producer): queued and applied at the start of the next process().
  bool setParameterNormalized(unsigned id, double value) override;
  bool automateParameter(unsigned id, double value) override;
  bool supportsAutomation() const override { return true; }
  std::string parameterText(unsigned id, double normalized) const override;

  // MIDI: an instrument (a component with an event input bus) gets note-on/off with sample offsets
  // as VST3 events; controllers and pitch bend go in through the plugin's IMidiMapping as
  // parameter changes at the same offsets. Events are applied in the next process() call.
  bool acceptsMidi() const override;
  void handleMidi(const MidiEvent* events, std::size_t count) override;

 private:
  Vst3Instance();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wezt::plugins
