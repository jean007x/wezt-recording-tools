#include "edit/beat_detective.h"

#include <algorithm>
#include <cmath>

#include "edit/clip_tools.h"

namespace wezt::edit {

BeatDetectiveResult beatDetective(const std::vector<Clip>& playlist, double sr, const BeatDetectiveParams& p) {
  BeatDetectiveResult r;
  r.clips = playlist;
  if (p.bpm <= 0 || p.gridBeats <= 0 || sr <= 0) return r;
  const double gridF = p.gridBeats * 60.0 / p.bpm * sr;
  const std::int64_t pre = static_cast<std::int64_t>(p.preMs * 0.001 * sr);
  const std::int64_t fade = static_cast<std::int64_t>(p.fadeMs * 0.001 * sr);
  const std::int64_t minSlice = static_cast<std::int64_t>(0.02 * sr);
  const auto hits = transientPositions(playlist, sr, p.sensitivity);

  // 1. cut every unlocked clip at its hits
  std::vector<Clip> pieces;
  std::vector<std::int64_t> hitAt;   // detected hit of each piece (timeline frame), or -1 if none
  int nextId = 1;
  for (const Clip& c : playlist) nextId = std::max(nextId, c.id + 1);
  for (const Clip& c0 : playlist) {
    if (c0.locked || c0.muted || !c0.audio) { pieces.push_back(c0); hitAt.push_back(-1); continue; }
    std::vector<std::int64_t> cuts;
    for (std::int64_t h : hits) if (h > c0.start + minSlice && h < c0.start + c0.length - minSlice) cuts.push_back(h);
    Clip cur = c0;
    std::int64_t curHit = -1;
    // the clip's own start counts as a hit when it begins with sound
    {
      const std::size_t from = static_cast<std::size_t>(std::clamp<std::int64_t>(c0.offset, 0, static_cast<std::int64_t>(c0.audio->frames)));
      const std::size_t to = std::min(c0.audio->frames, from + static_cast<std::size_t>(0.01 * sr));
      float pk = 0.f; for (std::size_t ch = 0; ch < c0.audio->channels; ++ch) for (std::size_t i = from; i < to; ++i) pk = std::max(pk, std::fabs(c0.audio->channel(ch)[i]));
      if (pk > 0.05f * std::max(clipPeak(c0), 1e-6f)) curHit = c0.start;
    }
    for (std::int64_t h : cuts) {
      Clip left, right;
      const std::int64_t at = std::max(cur.start + minSlice, h - pre);
      if (!separateAt(cur, at, left, right)) continue;
      left.id = cur.id;
      pieces.push_back(left); hitAt.push_back(curHit);
      right.id = nextId++;
      cur = right; curHit = h;
    }
    pieces.push_back(cur); hitAt.push_back(curHit);
  }

  // 2. move each slice toward the grid
  double sum = 0;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (hitAt[i] < 0 || pieces[i].locked) continue;
    ++r.hits;
    const double step = static_cast<double>(hitAt[i]) / gridF;
    double line = std::round(step);
    double target = line * gridF;
    if (p.swing > 0 && (static_cast<long long>(line) & 1)) target += p.swing * 0.5 * gridF;
    const double dist = target - static_cast<double>(hitAt[i]);
    if (p.windowBeats > 0 && std::fabs(dist) > p.windowBeats * 60.0 / p.bpm * sr) continue;
    std::int64_t shift = static_cast<std::int64_t>(std::llround(dist * p.strength));
    if (pieces[i].start + shift < 0) shift = -pieces[i].start;
    if (shift == 0) continue;
    pieces[i].start += shift;
    ++r.moved;
    const double ms = std::fabs(static_cast<double>(shift)) / sr * 1000.0;
    sum += ms; r.maxShiftMs = std::max(r.maxShiftMs, ms);
  }
  if (r.moved) r.meanShiftMs = sum / r.moved;

  // 3. smooth the joins: trim overlaps and fade the edges that meet
  std::sort(pieces.begin(), pieces.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
  for (std::size_t i = 0; i + 1 < pieces.size(); ++i) {
    Clip& a = pieces[i]; const Clip& b = pieces[i + 1];
    if (a.locked || b.locked || !a.audio || !b.audio) continue;
    if (a.start + a.length > b.start) a.length = std::max<std::int64_t>(minSlice / 2, b.start + fade - a.start);
    if (a.start + a.length >= b.start) { a.fadeOut = std::min<std::int64_t>(fade, a.length / 2); }
    pieces[i + 1].fadeIn = std::max(pieces[i + 1].fadeIn, std::min<std::int64_t>(fade, pieces[i + 1].length / 2));
  }
  r.clips = std::move(pieces);
  return r;
}

}  // namespace wezt::edit
