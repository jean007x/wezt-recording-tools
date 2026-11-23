#pragma once
// Cutting clips at frame positions: the building blocks of copy, cut, paste, repeat, insert silence and consolidate.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "session/strip.h"
#include "session/track_source.h"

namespace wezt::session {

// The part of a clip that lies inside [a, b) on the timeline (a and b already clamped to the clip). Fades stay only on the edges that
// are still the clip's own edges; the gain line is cut to the same part.
inline Clip slicePiece(const Clip& c, std::int64_t a, std::int64_t b) {
  Clip n = c;
  const std::int64_t shift = a - c.start;
  n.start = a; n.offset = c.offset + shift; n.length = b - a;
  if (a > c.start) n.fadeIn = 0;
  if (b < c.start + c.length) n.fadeOut = 0;
  if (!c.gainEnv.empty() && shift > 0) {
    AutomationLane env;
    float held = 0.f; bool have = false;
    for (const auto& p : c.gainEnv.points()) {
      if (p.frame <= shift) { held = p.value; have = true; }
      else if (p.frame - shift < n.length) env.add(p.frame - shift, p.value);
    }
    if (have) env.add(0, held);
    n.gainEnv = env;
  }
  return n;
}

// Copies the material in [a, b) of a track: every clip that touches it, trimmed to it, starting relative to a.
inline std::vector<Clip> copyRange(const std::vector<Clip>& list, std::int64_t a, std::int64_t b) {
  std::vector<Clip> out;
  for (const auto& c : list) {
    const std::int64_t s = std::max(c.start, a), e = std::min(c.start + c.length, b);
    if (e <= s) continue;
    Clip n = slicePiece(c, s, e);
    n.start -= a;
    out.push_back(std::move(n));
  }
  return out;
}

// Takes [a, b) out of a track's playlist, leaving what lies outside as one or two pieces. Locked clips stay as they are.
inline std::vector<Clip> removeRange(int& nextClipId, std::vector<Clip> list, std::int64_t a, std::int64_t b) {
  std::vector<Clip> out;
  for (auto& c : list) {
    const std::int64_t cs = c.start, ce = c.start + c.length;
    if (c.locked || ce <= a || cs >= b) { out.push_back(std::move(c)); continue; }
    if (cs < a) { Clip l = slicePiece(c, cs, a); out.push_back(std::move(l)); }
    if (ce > b) { Clip r = slicePiece(c, b, ce); r.id = nextClipId++; out.push_back(std::move(r)); }
  }
  return out;
}


}  // namespace wezt::session
