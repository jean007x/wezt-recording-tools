#pragma once
// Builds the plug-in instance a saved or chosen descriptor stands for: Wezt's own instruments first, then (when this build has the
// loaders) VST3 and CLAP plug-ins from disk. Returns null with a reason when it cannot.
#include <memory>
#include <string>

#include "plugins/plugin_host.h"

namespace wezt::ui {

std::unique_ptr<plugins::IPluginInstance> makeInsert(const plugins::PluginDescriptor& d, std::string* error = nullptr);
// True when this build can load third-party plug-ins at all.
bool thirdPartyHosting();

}  // namespace wezt::ui
