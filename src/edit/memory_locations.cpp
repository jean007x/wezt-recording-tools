#include "edit/memory_locations.h"

#include <algorithm>

namespace wezt::edit {

int MemoryLocations::add(MemoryLocation m, int number) {
  if (m.end < m.start) m.end = m.start;
  if (m.start < 0) { m.end -= m.start; m.start = 0; }
  if (number <= 0 || find(number)) {
    number = 1;
    while (find(number)) ++number;
  }
  m.number = number;
  list_.push_back(std::move(m));
  return number;
}
bool MemoryLocations::remove(int number) {
  auto it = std::find_if(list_.begin(), list_.end(), [&](const MemoryLocation& m) { return m.number == number; });
  if (it == list_.end()) return false;
  list_.erase(it);
  return true;
}
MemoryLocation* MemoryLocations::find(int number) {
  for (auto& m : list_) if (m.number == number) return &m;
  return nullptr;
}
const MemoryLocation* MemoryLocations::find(int number) const {
  for (auto& m : list_) if (m.number == number) return &m;
  return nullptr;
}
std::vector<MemoryLocation> MemoryLocations::sorted() const {
  auto v = list_;
  std::sort(v.begin(), v.end(), [](const MemoryLocation& a, const MemoryLocation& b) { return a.start != b.start ? a.start < b.start : a.number < b.number; });
  return v;
}
const MemoryLocation* MemoryLocations::next(std::int64_t frame) const {
  const MemoryLocation* best = nullptr;
  for (auto& m : list_) if (m.start > frame && (!best || m.start < best->start || (m.start == best->start && m.number < best->number))) best = &m;
  return best;
}
const MemoryLocation* MemoryLocations::previous(std::int64_t frame) const {
  const MemoryLocation* best = nullptr;
  for (auto& m : list_) if (m.start < frame && (!best || m.start > best->start || (m.start == best->start && m.number < best->number))) best = &m;
  return best;
}

}  // namespace wezt::edit
