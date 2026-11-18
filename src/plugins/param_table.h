#pragma once
// A small, reusable parameter table for built-in plugins. It holds each parameter's plain value in
// an atomic (the control thread writes, the audio thread reads), maps plain <-> normalized, and
// formats display text, so a built-in instrument or effect gets the universal control surface
// (IPluginInstance::parameters / parameterNormalized / setParameterNormalized / parameterText)
// with a few lines per parameter.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "plugins/plugin_host.h"

namespace wezt::plugins {

struct ParamDef {
  const char* name;
  const char* unit;      // "dB", "Hz", "ms", "s", "%", "" ...
  double min, max, def;  // plain
  int steps = 0;         // 0 continuous, N = N+1 positions
  bool log = false;      // exponential mapping (min must be > 0)
  const char* semantic = "";
  const char* group = "";
};

class ParamTable {
 public:
  explicit ParamTable(std::vector<ParamDef> defs) : defs_(std::move(defs)), v_(defs_.size()) { for (std::size_t i = 0; i < defs_.size(); ++i) v_[i].store(defs_[i].def, std::memory_order_relaxed); }
  ParamTable(const ParamTable&) = delete;
  ParamTable& operator=(const ParamTable&) = delete;

  std::size_t size() const { return defs_.size(); }
  const ParamDef& def(std::size_t i) const { return defs_[i]; }
  double plain(std::size_t i) const { return v_[i].load(std::memory_order_relaxed); }  // audio thread
  float f(std::size_t i) const { return static_cast<float>(plain(i)); }

  double toNormalized(std::size_t i, double plainValue) const {
    const ParamDef& d = defs_[i];
    if (d.max == d.min) return 0.0;
    const double v = std::clamp(plainValue, std::min(d.min, d.max), std::max(d.min, d.max));
    if (d.log && d.min > 0) return std::log(v / d.min) / std::log(d.max / d.min);
    return (v - d.min) / (d.max - d.min);
  }
  double toPlain(std::size_t i, double normalized) const {
    const ParamDef& d = defs_[i];
    double n = std::clamp(std::isfinite(normalized) ? normalized : 0.0, 0.0, 1.0);
    if (d.steps > 0) n = std::round(n * d.steps) / d.steps;
    if (d.log && d.min > 0) return d.min * std::pow(d.max / d.min, n);
    return d.min + n * (d.max - d.min);
  }
  void setNormalized(std::size_t i, double n) { v_[i].store(toPlain(i, n), std::memory_order_relaxed); }
  void setPlain(std::size_t i, double p) { setNormalized(i, toNormalized(i, p)); }

  std::string text(std::size_t i, double normalized) const {
    const double p = toPlain(i, normalized);
    char buf[64];
    const char* u = defs_[i].unit;
    if (defs_[i].steps > 0 && defs_[i].max - defs_[i].min <= 16 && !*u) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(p)));
    else if (std::fabs(p) >= 1000 && !std::string(u).empty() && std::string(u) == "Hz") std::snprintf(buf, sizeof buf, "%.2f kHz", p / 1000.0);
    else std::snprintf(buf, sizeof buf, "%.*f%s%s", std::fabs(p) >= 100 ? 0 : (std::fabs(p) >= 10 ? 1 : 2), p, *u ? " " : "", u);
    return buf;
  }

  std::vector<ParamInfo> infos() const {
    std::vector<ParamInfo> out;
    for (std::size_t i = 0; i < defs_.size(); ++i) {
      const ParamDef& d = defs_[i];
      ParamInfo p;
      p.id = static_cast<unsigned>(i);
      p.name = d.name;
      p.defaultNormalized = toNormalized(i, d.def);
      p.min = d.min; p.max = d.max; p.unit = d.unit; p.steps = d.steps; p.logScale = d.log; p.semantic = d.semantic; p.group = d.group;
      out.push_back(std::move(p));
    }
    return out;
  }
  // "id=value" lines (plain values), for saveState/loadState.
  std::string serialize() const {
    std::string s;
    for (std::size_t i = 0; i < defs_.size(); ++i) { char b[96]; std::snprintf(b, sizeof b, "%zu=%.9g\n", i, plain(i)); s += b; }
    return s;
  }
  bool deserialize(const std::string& text) {
    std::size_t pos = 0; bool any = false;
    while (pos < text.size()) {
      const std::size_t nl = text.find('\n', pos);
      const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? text.size() : nl + 1;
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      char* end = nullptr;
      const unsigned long id = std::strtoul(line.c_str(), &end, 10);
      const double v = std::strtod(line.c_str() + eq + 1, nullptr);
      if (end == line.c_str() || id >= defs_.size() || !std::isfinite(v)) continue;
      setPlain(id, v); any = true;
    }
    return any;
  }

 private:
  std::vector<ParamDef> defs_;
  std::vector<std::atomic<double>> v_;
};

}  // namespace wezt::plugins
