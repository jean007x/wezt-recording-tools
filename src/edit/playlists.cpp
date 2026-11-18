#include "edit/playlists.h"

#include <algorithm>

#include "edit/clip_tools.h"

namespace wezt::edit {
using session::Clip;

namespace {
// The part of `c` that lies inside [a, b), or nothing.
bool slice(const Clip& c, std::int64_t a, std::int64_t b, Clip& out) {
  const std::int64_t s = std::max(c.start, a), e = std::min(c.start + c.length, b);
  if (e <= s) return false;
  out = c;
  Clip left, right, mid;
  Clip cur = c;
  if (s > cur.start) { if (!separateAt(cur, s, left, right)) return false; cur = right; }
  if (e < cur.start + cur.length) { if (!separateAt(cur, e, mid, right)) return false; cur = mid; }
  out = cur;
  return true;
}
}  // namespace

std::vector<Clip> compRange(const std::vector<Clip>& live, const std::vector<Clip>& alt, std::int64_t from, std::int64_t to) {
  std::vector<Clip> out;
  if (to <= from) return live;
  int nextId = 1;
  for (const Clip& c : live) nextId = std::max(nextId, c.id + 1);
  for (const Clip& c : live) {
    if (c.locked || c.start + c.length <= from || c.start >= to) { out.push_back(c); continue; }
    Clip part;
    if (c.start < from && slice(c, c.start, from, part)) out.push_back(part);                        // before the comped range
    if (c.start + c.length > to && slice(c, to, c.start + c.length, part)) { part.id = nextId++; out.push_back(part); }   // after it
  }
  for (const Clip& c : alt) {
    Clip part;
    if (c.muted || !slice(c, from, to, part)) continue;
    part.id = nextId++;
    out.push_back(part);
  }
  std::sort(out.begin(), out.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
  return out;
}

}  // namespace wezt::edit
