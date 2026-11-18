#pragma once
// Finds and installs plugins of every format Wezt knows about, and says honestly what it can
// do with each one. Nothing here loads or executes a plugin: scanning reads directory entries
// and file headers only.
#include <optional>
#include <string>
#include <vector>

#include "plugins/plugin_host.h"

namespace wezt::plugins {

enum class Hostability {
  Native,       // loads in-process
  NeedsBridge,  // 32-bit plugin: runs in a helper process (see bridge/)
  NotBuilt,     // a format we intend to host, but this build lacks the loader
  NotHostable   // Pro Tools-only or otherwise impossible for a third-party host
};

struct HostCapabilities {
  bool vst3 = false, vst2 = false, clap = false, au = false, bridge = false;
  static HostCapabilities forThisBuild();  // reads the WEZT_HAS_* compile definitions
};

struct LibraryEntry {
  PluginDescriptor plugin;
  Hostability hostability = Hostability::NotBuilt;
  std::string reason;                     // why, in plain words
  std::optional<PluginDescriptor> twin;   // a hostable copy of the same plugin, if installed
};

std::string formatName(Format f);

// 32 or 64 from an ELF / PE / Mach-O header (or a VST3 bundle's folder layout); 0 if unknown.
int detectBitness(const std::string& path);

Hostability classify(Format f, int bitness, const HostCapabilities& caps, std::string* reason);

class PluginLibrary {
 public:
  explicit PluginLibrary(HostCapabilities caps = HostCapabilities::forThisBuild()) : caps_(caps) {}

  // Standard system and per-user plugin folders for every format on this OS.
  static std::vector<std::string> defaultSearchPaths();

  void scan(const std::vector<std::string>& dirs);
  const std::vector<LibraryEntry>& entries() const { return entries_; }

  struct InstallResult {
    bool ok = false;
    std::string installedPath;
    std::string message;
  };
  // Copies a plugin file or bundle into the per-user folder for its format. It does not run
  // vendor installers (.exe/.msi/.pkg) and refuses Pro Tools-only formats. `rootOverride`
  // replaces the home/AppData root (used by tests).
  InstallResult install(const std::string& source, bool overwrite = false, const std::string& rootOverride = {}) const;

  static std::string userPluginDir(Format f, const std::string& rootOverride = {});

 private:
  HostCapabilities caps_;
  std::vector<LibraryEntry> entries_;
};

}  // namespace wezt::plugins
