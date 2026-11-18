#pragma once
// Memory locations: numbered, named positions or ranges with a comment, recalled by number.
// Also used for markers. Stored in the session file.
#include <cstdint>
#include <string>
#include <vector>

namespace wezt::edit {

struct MemoryLocation {
  int number = 0;                 // 1.. ; unique
  std::string name, comment;
  std::int64_t start = 0, end = 0;  // end == start for a marker; end > start for a selection
  int zoomPreset = 0;             // 0 = unchanged
  std::vector<int> visibleTracks; // empty = leave track visibility alone
  std::int64_t preRoll = 0, postRoll = 0;
  bool isRange() const { return end > start; }
};

class MemoryLocations {
 public:
  // Adds at the first free number (or `number` when it is free and > 0). Returns the number.
  int add(MemoryLocation m, int number = 0);
  bool remove(int number);
  MemoryLocation* find(int number);
  const MemoryLocation* find(int number) const;
  // Sorted by position; ties by number.
  std::vector<MemoryLocation> sorted() const;
  const std::vector<MemoryLocation>& all() const { return list_; }
  // Next/previous location strictly after/before a position. Returns nullptr when none.
  const MemoryLocation* next(std::int64_t frame) const;
  const MemoryLocation* previous(std::int64_t frame) const;
  void clear() { list_.clear(); }

 private:
  std::vector<MemoryLocation> list_;
};

}  // namespace wezt::edit
