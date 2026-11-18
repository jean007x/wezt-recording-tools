#pragma once
// Creates Wezt's own instruments from a saved descriptor, so session load, track duplication and the
// UI all agree on what "built-in" means.
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dsp/effect_plugins.h"
#include "instruments/sampler.h"
#include "instruments/wezt_synth.h"

namespace wezt::instruments {

inline std::unique_ptr<plugins::IPluginInstance> makeBuiltin(const plugins::PluginDescriptor& d) {
  if (d.format != plugins::Format::Builtin) return nullptr;
  if (d.name == "Wezt Sampler") return std::make_unique<Sampler>();
  if (d.name == "Wezt Synth") return std::make_unique<WeztSynth>();
  if (d.name == "Wezt EQ") return std::make_unique<dsp::EqPlugin>();
  if (d.name == "Wezt Compressor") return std::make_unique<dsp::CompressorPlugin>();
  if (d.name == "Wezt Limiter") return std::make_unique<dsp::LimiterPlugin>();
  if (d.name == "Wezt Stereo Width") return std::make_unique<dsp::WidthPlugin>();
  return nullptr;
}

// Everything makeBuiltin can build, for menus and agents: {name, true when it is an instrument}.
inline std::vector<std::pair<std::string, bool>> builtinPlugins() {
  return {{"Wezt Synth", true}, {"Wezt Sampler", true}, {"Wezt EQ", false}, {"Wezt Compressor", false}, {"Wezt Limiter", false}, {"Wezt Stereo Width", false}};
}

}  // namespace wezt::instruments
