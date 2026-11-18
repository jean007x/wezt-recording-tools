#pragma once
// Style profiles: what a body of music measures like. A profile is built by feeding it the analysis
// of tracks (your own mixes, or reference tracks you supply). It keeps running statistics, so it
// improves with every track and never stores the audio. Profiles live in a folder on disk.
#include <array>
#include <map>
#include <string>
#include <vector>

#include "ai/features.h"

namespace wezt::ai {

class StyleProfile {
 public:
  StyleProfile() = default;
  explicit StyleProfile(std::string name) : name_(std::move(name)) {}
  const std::string& name() const { return name_; }
  int count() const { return count_; }
  void add(const Features& f);                  // ignores invalid analyses
  Features mean() const;
  std::vector<float> stddev() const;            // one per feature, 0 with fewer than 2 tracks
  // Most common key so far ({-1,false} when none) and the tempo range seen.
  std::pair<int, bool> commonKey() const;
  float minBpm() const { return minBpm_; }
  float maxBpm() const { return maxBpm_; }

  std::string serialize() const;
  static bool parse(const std::string& text, StyleProfile& out);

 private:
  std::string name_;
  int count_ = 0;
  std::vector<double> mean_, m2_;
  std::array<int, 24> keys_{};
  float minBpm_ = 0.f, maxBpm_ = 0.f;
};

// A folder of profiles. Names are reduced to safe file names; nothing outside the folder is touched.
class ProfileStore {
 public:
  explicit ProfileStore(std::string dir) : dir_(std::move(dir)) {}
  std::vector<std::string> list() const;
  bool load(const std::string& name, StyleProfile& out) const;
  bool save(const StyleProfile& p, std::string* error = nullptr) const;
  bool remove(const std::string& name) const;
  static std::string safeName(const std::string& name);  // empty if nothing usable remains
  const std::string& dir() const { return dir_; }

 private:
  std::string pathFor(const std::string& name) const;
  std::string dir_;
};

}  // namespace wezt::ai
