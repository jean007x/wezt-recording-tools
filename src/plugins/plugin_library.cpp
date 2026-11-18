#include "plugins/plugin_library.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "plugins/aax_twin.h"

namespace fs = std::filesystem;

namespace wezt::plugins {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string getenvStr(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

bool pathMentionsVst(const fs::path& p) {
  for (const auto& part : p) {
    const std::string s = lower(part.string());
    if (s.find("vst") != std::string::npos && s.find("vst3") == std::string::npos) return true;  // VST2 folders, not VST3 ones
  }
  return false;
}

struct Kind {
  bool known = false;
  Format format = Format::VST3;
  std::string note;
};

// `explicitUserIntent` lets a bare .dll count as VST2 (when the user hands us the file).
Kind kindFromPath(const fs::path& p, bool explicitUserIntent) {
  const std::string e = lower(p.extension().string());
  Kind k;
  k.known = true;
  if (e == ".vst3") k.format = Format::VST3;
  else if (e == ".clap") k.format = Format::CLAP;
  else if (e == ".component") k.format = Format::AU;
  else if (e == ".vst") k.format = Format::VST2;
  else if (e == ".aaxplugin") k.format = Format::AAX;
  else if (e == ".dpm") { k.format = Format::RTAS; k.note = "RTAS or TDM (both use .dpm; not opened to tell which)"; }
  else if (e == ".dll" && (explicitUserIntent || pathMentionsVst(p.parent_path()))) k.format = Format::VST2;
  else k.known = false;
  return k;
}

bool readBytes(const fs::path& p, std::size_t offset, void* dst, std::size_t n) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(static_cast<std::streamoff>(offset));
  f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
  return static_cast<std::size_t>(f.gcount()) == n;
}

int bitnessOfFile(const fs::path& p) {
  unsigned char h[64] = {};
  if (!readBytes(p, 0, h, 8)) return 0;
  // ELF
  if (h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') return h[4] == 1 ? 32 : h[4] == 2 ? 64 : 0;
  // PE: "MZ", e_lfanew at 0x3C, then "PE\0\0" and the Machine field.
  if (h[0] == 'M' && h[1] == 'Z') {
    unsigned char lf[4];
    if (!readBytes(p, 0x3C, lf, 4)) return 0;
    const std::uint32_t off = lf[0] | (lf[1] << 8) | (lf[2] << 16) | (static_cast<std::uint32_t>(lf[3]) << 24);
    unsigned char pe[6];
    if (off > (1u << 20) || !readBytes(p, off, pe, 6)) return 0;
    if (!(pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0)) return 0;
    const unsigned machine = pe[4] | (pe[5] << 8);
    if (machine == 0x014C) return 32;                     // i386
    if (machine == 0x8664 || machine == 0xAA64) return 64;  // x64, arm64
    return 0;
  }
  // Mach-O thin (little endian)
  if (h[0] == 0xCF && h[1] == 0xFA && h[2] == 0xED && h[3] == 0xFE) return 64;
  if (h[0] == 0xCE && h[1] == 0xFA && h[2] == 0xED && h[3] == 0xFE) return 32;
  // Mach-O universal (big endian): any 64-bit slice wins.
  if (h[0] == 0xCA && h[1] == 0xFE && h[2] == 0xBA && h[3] == 0xBE) {
    const unsigned n = (h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7];
    if (n == 0 || n > 16) return 0;
    int best = 0;
    for (unsigned i = 0; i < n; ++i) {
      unsigned char a[4];
      if (!readBytes(p, 8 + static_cast<std::size_t>(i) * 20, a, 4)) break;
      const std::uint32_t cpu = (static_cast<std::uint32_t>(a[0]) << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
      if (cpu & 0x01000000u) best = 64;
      else if (best == 0) best = 32;
    }
    return best;
  }
  return 0;
}

}  // namespace

HostCapabilities HostCapabilities::forThisBuild() {
  HostCapabilities c;
#ifdef WEZT_HAS_VST3
  c.vst3 = true;
#endif
#ifdef WEZT_HAS_VST2
  c.vst2 = true;
#endif
#ifdef WEZT_HAS_CLAP
  c.clap = true;
#endif
#ifdef WEZT_HAS_BRIDGE
  c.bridge = true;
#endif
  return c;
}

std::string formatName(Format f) {
  switch (f) {
    case Format::VST3: return "VST3";
    case Format::VST2: return "VST2";
    case Format::AU: return "AU";
    case Format::AAX: return "AAX";
    case Format::CLAP: return "CLAP";
    case Format::RTAS: return "RTAS/TDM";
    case Format::TDM: return "TDM";
    case Format::Bridged32: return "32-bit (bridged)";
    case Format::Builtin: return "Built-in";
  }
  return "?";
}

int detectBitness(const std::string& path) {
  std::error_code ec;
  const fs::path p(path);
  if (fs::is_directory(p, ec)) {
    const fs::path c = p / "Contents";
    if (fs::exists(c / "x86_64-win", ec) || fs::exists(c / "arm64-win", ec)) return 64;
    if (fs::exists(c / "x86-win", ec)) return 32;
    for (const char* sub : {"MacOS", "x86_64-linux", "aarch64-linux", "i386-linux"}) {
      const fs::path d = c / sub;
      if (!fs::is_directory(d, ec)) continue;
      for (const auto& e : fs::directory_iterator(d, ec))
        if (e.is_regular_file(ec)) {
          const int b = bitnessOfFile(e.path());
          if (b) return b;
        }
    }
    return 0;
  }
  return bitnessOfFile(p);
}

Hostability classify(Format f, int bitness, const HostCapabilities& caps, std::string* reason) {
  auto say = [&](const char* s) { if (reason) *reason = s; };
  switch (f) {
    case Format::AAX:
      say("Pro Tools-only. Built against Avid's AAX SDK; Avid publishes no host SDK for other applications.");
      return Hostability::NotHostable;
    case Format::RTAS:
    case Format::TDM:
      say("Legacy Pro Tools plugin (.dpm). RTAS needs Pro Tools' own host interfaces and is 32-bit; TDM runs on Avid HD DSP "
          "cards. Neither can be hosted by other software.");
      return Hostability::NotHostable;
    case Format::Builtin:
      say("Ships with Wezt.");
      return Hostability::Native;
    case Format::Bridged32:
      say("Internal marker, not a file format.");
      return Hostability::NotHostable;
    case Format::VST3:
    case Format::VST2: {
      const bool built = f == Format::VST3 ? caps.vst3 : caps.vst2;
      if (!built) {
        say(f == Format::VST3 ? "This build has no VST3 loader (configure with WEZT_ENABLE_VST3)."
                              : "This build has no VST2 loader (configure with WEZT_ENABLE_VST2).");
        return Hostability::NotBuilt;
      }
      if (bitness == 32) {
        if (caps.bridge) { say("32-bit plugin: runs in the bridge helper process."); return Hostability::NeedsBridge; }
        say("32-bit plugin and this build has no bridge.");
        return Hostability::NotHostable;
      }
      say("Loads in-process.");
      return Hostability::Native;
    }
    case Format::CLAP:
      if (!caps.clap) { say("CLAP hosting is not implemented yet."); return Hostability::NotBuilt; }
      say("Loads in-process.");
      return Hostability::Native;
    case Format::AU:
      if (!caps.au) { say("AU hosting is not implemented yet (macOS only)."); return Hostability::NotBuilt; }
      say("Loads in-process.");
      return Hostability::Native;
  }
  return Hostability::NotHostable;
}

std::vector<std::string> PluginLibrary::defaultSearchPaths() {
  std::vector<std::string> v;
#if defined(_WIN32)
  const std::string pf = getenvStr("ProgramFiles"), pf86 = getenvStr("ProgramFiles(x86)"), la = getenvStr("LOCALAPPDATA");
  const std::string pfBase = pf.empty() ? "C:\\Program Files" : pf, pf86Base = pf86.empty() ? "C:\\Program Files (x86)" : pf86;
  for (const std::string& b : {pfBase, pf86Base}) {
    v.push_back(b + "\\Common Files\\VST3");
    v.push_back(b + "\\VSTPlugins");  // 32-bit VST2 plugins usually live under Program Files (x86)
    v.push_back(b + "\\Steinberg\\VSTPlugins");
    v.push_back(b + "\\Common Files\\CLAP");
    v.push_back(b + "\\Common Files\\Avid\\Audio\\Plug-Ins");       // AAX (listed, never loaded)
    v.push_back(b + "\\Common Files\\Digidesign\\DAE\\Plug-Ins");   // RTAS / TDM (listed, never loaded)
  }
  if (!la.empty()) {
    v.push_back(la + "\\Programs\\Common\\VST3");
    v.push_back(la + "\\Programs\\Common\\CLAP");
    v.push_back(la + "\\Wezt\\VST2");
  }
#elif defined(__APPLE__)
  const std::string home = getenvStr("HOME");
  for (const std::string& b : {std::string(), home}) {
    const std::string r = b + "/Library/Audio/Plug-Ins/";
    for (const char* d : {"VST3", "VST", "Components", "CLAP"}) v.push_back(r + d);
  }
  v.push_back("/Library/Application Support/Avid/Audio/Plug-Ins");       // AAX (listed, never loaded)
  v.push_back("/Library/Application Support/Digidesign/Plug-Ins");       // RTAS / TDM (listed, never loaded)
#else
  const std::string home = getenvStr("HOME");
  for (const char* d : {"/usr/lib/vst3", "/usr/local/lib/vst3", "/usr/lib/clap", "/usr/local/lib/clap", "/usr/lib/vst"}) v.push_back(d);
  if (!home.empty()) for (const char* d : {"/.vst3", "/.clap", "/.vst"}) v.push_back(home + d);
#endif
  return v;
}

void PluginLibrary::scan(const std::vector<std::string>& dirs) {
  entries_.clear();
  std::error_code ec;
  for (const auto& d : dirs) {
    if (!fs::is_directory(d, ec)) continue;
    fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const Kind k = kindFromPath(it->path(), false);
      if (!k.known) continue;
      if (it->is_directory(ec)) it.disable_recursion_pending();  // don't walk into bundles
      LibraryEntry e;
      e.plugin.name = it->path().stem().string();
      e.plugin.path = it->path().string();
      e.plugin.format = k.format;
      e.plugin.note = k.note;
      e.plugin.bitness = detectBitness(e.plugin.path);
      e.plugin.is32bit = e.plugin.bitness == 32;
      e.hostability = classify(k.format, e.plugin.bitness, caps_, &e.reason);
      entries_.push_back(std::move(e));
    }
  }
  // For plugins we can't host, point at a hostable copy of the same plugin if one is installed.
  auto rank = [](Format f) { return f == Format::VST3 ? 0 : f == Format::CLAP ? 1 : f == Format::AU ? 2 : 3; };
  for (auto& e : entries_) {
    if (e.hostability != Hostability::NotHostable && e.hostability != Hostability::NotBuilt) continue;
    const std::string key = normalizePluginName(e.plugin.name);
    if (key.empty()) continue;
    const LibraryEntry* best = nullptr;
    for (const auto& o : entries_) {
      if (&o == &e || (o.hostability != Hostability::Native && o.hostability != Hostability::NeedsBridge)) continue;
      if (normalizePluginName(o.plugin.name) != key) continue;
      if (!best || rank(o.plugin.format) < rank(best->plugin.format)) best = &o;
    }
    if (best) e.twin = best->plugin;
  }
}

std::string PluginLibrary::userPluginDir(Format f, const std::string& rootOverride) {
#if defined(_WIN32)
  const std::string root = rootOverride.empty() ? getenvStr("LOCALAPPDATA") : rootOverride;
  if (root.empty()) return {};
  switch (f) {
    case Format::VST3: return root + "\\Programs\\Common\\VST3";
    case Format::CLAP: return root + "\\Programs\\Common\\CLAP";
    case Format::VST2: return root + "\\Wezt\\VST2";
    default: return {};
  }
#elif defined(__APPLE__)
  const std::string root = rootOverride.empty() ? getenvStr("HOME") : rootOverride;
  if (root.empty()) return {};
  const std::string r = root + "/Library/Audio/Plug-Ins/";
  switch (f) {
    case Format::VST3: return r + "VST3";
    case Format::CLAP: return r + "CLAP";
    case Format::AU: return r + "Components";
    case Format::VST2: return r + "VST";
    default: return {};
  }
#else
  const std::string root = rootOverride.empty() ? getenvStr("HOME") : rootOverride;
  if (root.empty()) return {};
  switch (f) {
    case Format::VST3: return root + "/.vst3";
    case Format::CLAP: return root + "/.clap";
    case Format::VST2: return root + "/.vst";
    default: return {};
  }
#endif
}

PluginLibrary::InstallResult PluginLibrary::install(const std::string& source, bool overwrite, const std::string& rootOverride) const {
  InstallResult r;
  std::error_code ec;
  // "Foo.vst3/" (what shell tab-completion gives for a bundle) has an empty filename: strip the separator.
  fs::path src = fs::path(source).lexically_normal();
  if (src.filename().empty()) src = src.parent_path();
  if (!fs::exists(src, ec)) { r.message = "not found: " + source; return r; }

  const std::string ext = lower(src.extension().string());
  if (ext == ".exe" || ext == ".msi" || ext == ".pkg" || ext == ".dmg" || ext == ".zip" || ext == ".7z") {
    r.message = "That is an installer or archive. Run or unpack it first, then point me at the plugin file itself.";
    return r;
  }
  const Kind k = kindFromPath(src, /*explicitUserIntent=*/true);
  if (!k.known) { r.message = "unrecognised plugin type: " + src.filename().string(); return r; }
  if (k.format == Format::AAX || k.format == Format::RTAS || k.format == Format::TDM) {
    r.message = formatName(k.format) + " plugins only run inside Pro Tools. Install them with the vendor's installer; Wezt cannot host them. " +
                "Look for a VST3 or AU version of the same plugin.";
    return r;
  }
  const std::string dir = userPluginDir(k.format, rootOverride);
  if (dir.empty()) { r.message = formatName(k.format) + " has no per-user plugin folder on this system."; return r; }

  fs::create_directories(dir, ec);
  if (ec) { r.message = "cannot create " + dir + ": " + ec.message(); return r; }
  const fs::path dest = fs::path(dir) / src.filename();
  const bool destExists = fs::exists(dest, ec);
  if (destExists) {
    // Installing a plugin that already is the installed copy: nothing to do. (Deleting "the old one"
    // here would delete the only copy.)
    if (fs::equivalent(src, dest, ec)) {
      r.ok = true;
      r.installedPath = dest.string();
      r.message = "already in the plugin folder: " + dest.string();
      return r;
    }
    if (!overwrite) { r.message = "already installed: " + dest.string(); r.installedPath = dest.string(); return r; }
  }

  // Copy next to the destination first; only swap once the copy is complete, so a failed copy
  // (disk full, unreadable file) leaves any existing installation untouched.
  const fs::path staging = fs::path(dest.string() + ".wezt-new");
  const fs::path backup = fs::path(dest.string() + ".wezt-old");
  fs::remove_all(staging, ec);
  fs::remove_all(backup, ec);
  if (fs::is_directory(src, ec))
    fs::copy(src, staging, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
  else
    fs::copy_file(src, staging, ec);
  if (ec) {
    r.message = "copy failed: " + ec.message();
    std::error_code ignore;
    fs::remove_all(staging, ignore);
    return r;
  }
  if (destExists) {
    fs::rename(dest, backup, ec);
    if (ec) { r.message = "cannot replace the existing plugin: " + ec.message(); fs::remove_all(staging, ec); return r; }
  }
  fs::rename(staging, dest, ec);
  if (ec) {
    r.message = "could not put the plugin in place: " + ec.message();
    std::error_code ignore;
    if (destExists) fs::rename(backup, dest, ignore);  // put the old one back
    fs::remove_all(staging, ignore);
    return r;
  }
  if (destExists) fs::remove_all(backup, ec);

  r.ok = true;
  r.installedPath = dest.string();
  const int bits = detectBitness(dest.string());
  std::string why;
  const Hostability h = classify(k.format, bits, caps_, &why);
  r.message = "installed to " + dest.string();
  if (h != Hostability::Native) r.message += " (" + why + ")";
  return r;
}

}  // namespace wezt::plugins
