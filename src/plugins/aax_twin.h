#pragma once
#include <optional>
#include <string>
#include <vector>

#include "plugins/plugin_host.h"

namespace wezt::plugins {

// Wezt Recording Tools does NOT load AAX binaries: AAX plugins are built against Avid's
// AAX SDK and only run inside Pro Tools, and Avid offers no third-party host SDK.
// What we can do legitimately is notice which of your AAX plugins also ship as a format we
// can host (VST3, CLAP, AU) and point you at that twin. This only reads directory entries; it
// never opens or executes an AAX plugin.

struct TwinMatch {
  PluginDescriptor aax;
  std::optional<PluginDescriptor> twin;  // empty when no hostable twin is installed
};

std::vector<std::string> defaultAaxSearchPaths();
std::vector<std::string> defaultHostableSearchPaths();  // VST3 / CLAP / AU locations

// Lists entries named *.aaxplugin (files on Windows, bundles on macOS) without loading them.
std::vector<PluginDescriptor> findAaxPlugins(const std::vector<std::string>& dirs);
// Lists *.vst3, *.clap and (macOS) *.component entries.
std::vector<PluginDescriptor> findHostablePlugins(const std::vector<std::string>& dirs);

// Lowercases and strips punctuation plus format words so "Foo Bar (AAX)" matches "foo_bar.vst3".
std::string normalizePluginName(const std::string& name);

std::vector<TwinMatch> resolveTwins(const std::vector<PluginDescriptor>& aax,
                                    const std::vector<PluginDescriptor>& hostable);

}  // namespace wezt::plugins
