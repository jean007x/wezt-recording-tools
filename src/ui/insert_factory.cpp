#include "ui/insert_factory.h"

#include "instruments/builtin.h"
#if defined(WEZT_HAS_VST3)
#include "plugins/vst3_host.h"
#endif
#if defined(WEZT_HAS_CLAP)
#include "plugins/clap_host.h"
#endif
#if defined(WEZT_HAS_BRIDGE)
#include "plugins/bridge/bridge_host.h"
#endif
#include <filesystem>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace wezt::ui {

std::string bridgeHelperPath() {
#if defined(WEZT_HAS_BRIDGE)
  namespace fs = std::filesystem;
  fs::path exe;
#if defined(_WIN32)
  wchar_t buf[4096]; const DWORD n = GetModuleFileNameW(nullptr, buf, 4096); if (n > 0 && n < 4096) exe = fs::path(std::wstring(buf, n));
  const char* name = "wezt-bridge.exe";
#elif defined(__APPLE__)
  char buf[4096]; uint32_t sz = sizeof buf; if (_NSGetExecutablePath(buf, &sz) == 0) exe = fs::path(buf);
  const char* name = "wezt-bridge";
#else
  std::error_code ec0; exe = fs::read_symlink("/proc/self/exe", ec0);
  const char* name = "wezt-bridge";
#endif
  if (exe.empty()) return {};
  std::error_code ec;
  const fs::path helper = exe.parent_path() / name;
  return fs::exists(helper, ec) ? helper.string() : std::string();
#else
  return {};
#endif
}

bool sandboxAvailable() {
#if defined(WEZT_HAS_BRIDGE)
  return !bridgeHelperPath().empty();
#else
  return false;
#endif
}

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
#if defined(WEZT_HAS_BRIDGE)
  if (d.sandbox && d.format == plugins::Format::VST3) {   // safe mode: the plug-in lives in its own process; its crash cannot take Wezt down
    const std::string helper = bridgeHelperPath();
    if (helper.empty()) { if (error) *error = "safe mode needs the wezt-bridge program next to Wezt, and it is missing"; return nullptr; }
    plugins::BridgeConfig cfg;
    cfg.helperExecutable = helper;
    return std::make_unique<plugins::BridgedPluginInstance>(cfg, d.path, 2);
  }
#endif
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
