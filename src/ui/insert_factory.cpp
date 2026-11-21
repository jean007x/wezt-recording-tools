#include "ui/insert_factory.h"

#include "instruments/builtin.h"
#if defined(WEZT_HAS_VST3)
#include "plugins/vst3_host.h"
#endif
#if defined(WEZT_HAS_CLAP)
#include "plugins/clap_host.h"
#endif

namespace wezt::ui {

bool thirdPartyHosting() {
#if defined(WEZT_HAS_VST3) || defined(WEZT_HAS_CLAP)
  return true;
#else
  return false;
#endif
}

std::unique_ptr<plugins::IPluginInstance> makeInsert(const plugins::PluginDescriptor& d, std::string* error) {
  std::string err;
  if (auto b = instruments::makeBuiltin(d)) return b;
  std::unique_ptr<plugins::IPluginInstance> out;
  switch (d.format) {
#if defined(WEZT_HAS_VST3)
    case plugins::Format::VST3:
      out = plugins::Vst3Instance::load(d.path, &err, d.name);
      if (!out) { std::string e2; out = plugins::Vst3Instance::load(d.path, &e2); if (out) err.clear(); }   // the name was a file name, not a class
      break;
#endif
#if defined(WEZT_HAS_CLAP)
    case plugins::Format::CLAP:
      out = plugins::ClapInstance::load(d.path, &err);
      break;
#endif
    default:
      err = "this build cannot load " + d.name + " (" + d.path + ")";
  }
  if (!out && error) *error = err.empty() ? "could not load " + d.path : err;
  return out;
}

}  // namespace wezt::ui
