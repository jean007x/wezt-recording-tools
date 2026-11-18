#include "plugins/aax_twin.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace wezt::plugins {

std::vector<std::string> defaultAaxSearchPaths() {
#if defined(_WIN32)
  return {"C:\\Program Files\\Common Files\\Avid\\Audio\\Plug-Ins"};
#elif defined(__APPLE__)
  return {"/Library/Application Support/Avid/Audio/Plug-Ins"};
#else
  return {};
#endif
}

std::vector<std::string> defaultHostableSearchPaths() {
#if defined(_WIN32)
  return {"C:\\Program Files\\Common Files\\VST3", "C:\\Program Files\\Common Files\\CLAP"};
#elif defined(__APPLE__)
  return {"/Library/Audio/Plug-Ins/VST3", "/Library/Audio/Plug-Ins/Components", "/Library/Audio/Plug-Ins/CLAP"};
#else
  return {"/usr/lib/vst3", "/usr/local/lib/vst3"};
#endif
}

namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::vector<PluginDescriptor> scanFor(const std::vector<std::string>& dirs, const std::string& ext, Format fmt) {
  std::vector<PluginDescriptor> out;
  std::error_code ec;
  for (const auto& d : dirs) {
    if (!fs::is_directory(d, ec)) continue;
    // Plugins can be nested in vendor folders, so walk recursively, but do not descend into
    // bundles (.vst3 / .component / .aaxplugin are directories on macOS).
    fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const auto& p = it->path();
      if (lower(p.extension().string()) != ext) continue;
      PluginDescriptor pd;
      pd.name = p.stem().string();
      pd.path = p.string();
      pd.format = fmt;
      out.push_back(pd);
      if (it->is_directory(ec)) it.disable_recursion_pending();
    }
  }
  return out;
}
}  // namespace

std::vector<PluginDescriptor> findAaxPlugins(const std::vector<std::string>& dirs) {
  return scanFor(dirs, ".aaxplugin", Format::AAX);
}

std::vector<PluginDescriptor> findHostablePlugins(const std::vector<std::string>& dirs) {
  auto v = scanFor(dirs, ".vst3", Format::VST3);
  auto c = scanFor(dirs, ".clap", Format::CLAP);
  auto a = scanFor(dirs, ".component", Format::AU);
  v.insert(v.end(), c.begin(), c.end());
  v.insert(v.end(), a.begin(), a.end());
  return v;
}

std::string normalizePluginName(const std::string& name) {
  std::string s = lower(name);
  for (const char* word : {"aax", "vst3", "clap", "x64", "64bit"}) {
    for (std::size_t pos; (pos = s.find(word)) != std::string::npos;) s.erase(pos, std::string(word).size());
  }
  std::string out;
  for (unsigned char c : s)
    if (std::isalnum(c)) out.push_back(static_cast<char>(c));
  return out;
}

std::vector<TwinMatch> resolveTwins(const std::vector<PluginDescriptor>& aax, const std::vector<PluginDescriptor>& hostable) {
  // Prefer VST3, then CLAP, then AU when several twins exist.
  auto rank = [](Format f) { return f == Format::VST3 ? 0 : f == Format::CLAP ? 1 : 2; };
  std::vector<TwinMatch> res;
  for (const auto& a : aax) {
    TwinMatch m;
    m.aax = a;
    const std::string key = normalizePluginName(a.name);
    const PluginDescriptor* best = nullptr;
    if (!key.empty()) {
      for (const auto& h : hostable) {
        if (normalizePluginName(h.name) != key) continue;
        if (!best || rank(h.format) < rank(best->format)) best = &h;
      }
    }
    if (best) m.twin = *best;
    res.push_back(std::move(m));
  }
  return res;
}

}  // namespace wezt::plugins
