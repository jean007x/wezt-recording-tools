#include "util/prefs.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace wezt {
namespace fs = std::filesystem;

std::string prefsPath() {
  auto env = [](const char* n) { const char* v = std::getenv(n); return std::string(v ? v : ""); };
  std::string dir = env("WEZT_CONFIG_DIR");
  if (dir.empty()) {
#if defined(_WIN32)
    const std::string a = env("APPDATA");
    if (!a.empty()) dir = a + "/Wezt";
#elif defined(__APPLE__)
    const std::string h = env("HOME");
    if (!h.empty()) dir = h + "/Library/Application Support/Wezt";
#else
    const std::string x = env("XDG_CONFIG_HOME"), h = env("HOME");
    if (!x.empty()) dir = x + "/wezt";
    else if (!h.empty()) dir = h + "/.config/wezt";
#endif
  }
  return dir.empty() ? std::string() : dir + "/settings.txt";
}

Prefs loadPrefs(const std::string& path) {
  Prefs p;
  if (path.empty()) return p;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "intro") p.introEnabled = v != "0";
    else if (k == "intro_logo") p.introLogoBlack = v != "gold";
    else if (k == "intro_sound") p.introSoundPath = v;
    else if (k == "click") p.clickOn = v == "1";
    else if (k == "count_off") { const int n = std::atoi(v.c_str()); p.countOffBars = (n == 1 || n == 2 || n == 4) ? n : 0; }
    else if (k == "pre_roll") { const int n = std::atoi(v.c_str()); p.preRollSeconds = (n == 2 || n == 4 || n == 8) ? n : 0; }
  }
  return p;
}

bool savePrefs(const Prefs& p, const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << "intro=" << (p.introEnabled ? 1 : 0) << "\n";
    f << "intro_logo=" << (p.introLogoBlack ? "black" : "gold") << "\n";
    f << "intro_sound=" << p.introSoundPath << "\n";
    f << "click=" << (p.clickOn ? 1 : 0) << "\n";
    f << "count_off=" << p.countOffBars << "\n";
    f << "pre_roll=" << p.preRollSeconds << "\n";
    if (!f) return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) { fs::remove(path, ec); ec.clear(); fs::rename(tmp, path, ec); }
  return !ec;
}

}  // namespace wezt
