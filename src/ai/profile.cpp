#include "ai/profile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace wezt::ai {
namespace fs = std::filesystem;

void StyleProfile::add(const Features& f) {
  if (!f.valid()) return;
  const auto v = toVector(f);
  if (mean_.empty()) { mean_.assign(v.size(), 0.0); m2_.assign(v.size(), 0.0); }
  ++count_;
  for (std::size_t i = 0; i < v.size(); ++i) {  // Welford's running mean and variance
    const double d = v[i] - mean_[i];
    mean_[i] += d / count_;
    m2_[i] += d * (v[i] - mean_[i]);
  }
  if (f.keyRoot >= 0 && f.keyConfidence > 0.f) ++keys_[static_cast<std::size_t>(f.keyRoot + (f.minor ? 12 : 0))];
  if (f.bpm > 0.f && f.bpmConfidence > 0.15f) {
    minBpm_ = minBpm_ == 0.f ? f.bpm : std::min(minBpm_, f.bpm);
    maxBpm_ = std::max(maxBpm_, f.bpm);
  }
}

Features StyleProfile::mean() const {
  std::vector<float> v(mean_.begin(), mean_.end());
  return v.empty() ? Features{} : fromVector(v);
}
std::vector<float> StyleProfile::stddev() const {
  std::vector<float> s(featureNames().size(), 0.f);
  if (count_ < 2) return s;
  for (std::size_t i = 0; i < m2_.size() && i < s.size(); ++i) s[i] = static_cast<float>(std::sqrt(m2_[i] / (count_ - 1)));
  return s;
}
std::pair<int, bool> StyleProfile::commonKey() const {
  const auto it = std::max_element(keys_.begin(), keys_.end());
  if (it == keys_.end() || *it == 0) return {-1, false};
  const int i = static_cast<int>(it - keys_.begin());
  return {i % 12, i >= 12};
}

std::string StyleProfile::serialize() const {
  std::ostringstream o;
  o.precision(9);
  o << "WEZT-PROFILE 1\nname " << name_ << "\ncount " << count_ << "\nbpm " << minBpm_ << " " << maxBpm_ << "\nkeys";
  for (int k : keys_) o << " " << k;
  o << "\nmean";
  for (double v : mean_) o << " " << v;
  o << "\nm2";
  for (double v : m2_) o << " " << v;
  o << "\nend\n";
  return o.str();
}

bool StyleProfile::parse(const std::string& text, StyleProfile& out) {
  std::istringstream is(text);
  std::string line;
  if (!std::getline(is, line) || line.rfind("WEZT-PROFILE 1", 0) != 0) return false;
  StyleProfile p;
  bool ended = false;
  const std::size_t n = featureNames().size();
  while (std::getline(is, line)) {
    std::istringstream ls(line);
    std::string key;
    ls >> key;
    if (key == "name") { std::getline(ls, p.name_); if (!p.name_.empty() && p.name_[0] == ' ') p.name_.erase(0, 1); }
    else if (key == "count") ls >> p.count_;
    else if (key == "bpm") ls >> p.minBpm_ >> p.maxBpm_;
    else if (key == "keys") { for (auto& k : p.keys_) ls >> k; }
    else if (key == "mean") { double v; while (ls >> v) p.mean_.push_back(v); }
    else if (key == "m2") { double v; while (ls >> v) p.m2_.push_back(v); }
    else if (key == "end") { ended = true; break; }
  }
  if (!ended || p.count_ < 0 || p.count_ > 10000000) return false;
  if (p.count_ > 0 && (p.mean_.size() != n || p.m2_.size() != n)) return false;
  for (double v : p.mean_) if (!std::isfinite(v)) return false;
  for (double v : p.m2_) if (!std::isfinite(v) || v < 0) return false;
  for (int k : p.keys_) if (k < 0) return false;
  if (p.count_ == 0) { p.mean_.clear(); p.m2_.clear(); }
  out = std::move(p);
  return true;
}

std::string ProfileStore::safeName(const std::string& name) {
  std::string o;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ') o += c;
    else if (c == '.') o += '_';
  }
  while (!o.empty() && o.front() == ' ') o.erase(o.begin());
  while (!o.empty() && o.back() == ' ') o.pop_back();
  if (o.size() > 80) o.resize(80);
  return o;
}
std::string ProfileStore::pathFor(const std::string& name) const { return (fs::path(dir_) / (safeName(name) + ".wprofile")).string(); }

std::vector<std::string> ProfileStore::list() const {
  std::vector<std::string> r;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir_, ec))
    if (e.is_regular_file(ec) && e.path().extension() == ".wprofile") r.push_back(e.path().stem().string());
  std::sort(r.begin(), r.end());
  return r;
}
bool ProfileStore::load(const std::string& name, StyleProfile& out) const {
  if (safeName(name).empty()) return false;
  std::ifstream f(pathFor(name), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  return StyleProfile::parse(ss.str(), out);
}
bool ProfileStore::save(const StyleProfile& p, std::string* error) const {
  if (safeName(p.name()).empty()) { if (error) *error = "profile needs a name"; return false; }
  std::error_code ec;
  fs::create_directories(dir_, ec);
  const std::string path = pathFor(p.name()), tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { if (error) *error = "cannot write " + tmp; return false; }
    const std::string t = p.serialize();
    f.write(t.data(), static_cast<std::streamsize>(t.size()));
    if (!f) { if (error) *error = "write failed"; return false; }
  }
  fs::rename(tmp, path, ec);
  if (ec) { if (error) *error = ec.message(); return false; }
  return true;
}
bool ProfileStore::remove(const std::string& name) const {
  if (safeName(name).empty()) return false;
  std::error_code ec;
  return fs::remove(pathFor(name), ec);
}

}  // namespace wezt::ai
