#pragma once
// Beat Detective: finds the hits in a track's clips, cuts the audio into one slice per hit, moves each
// slice toward the nearest grid line (with a strength control), and smooths the joins with short fades.
// Works on the playlist only; the audio itself is not changed, so it can be undone by restoring the
// playlist.
#include <vector>

#include "session/track_source.h"

namespace wezt::edit {
using session::Clip;

struct BeatDetectiveParams {
  double bpm = 120;
  double gridBeats = 0.25;     // 1 = quarter notes, 0.5 = eighths, 0.25 = sixteenths
  float strength = 1.f;        // 0 = leave, 1 = onto the grid
  float sensitivity = 0.5f;    // hit detection, 0..1
  double swing = 0;            // 0..1: delays every second grid step (0.5 = triplet feel at 8ths)
  double windowBeats = 0;      // only hits within this distance of a grid line move (0 = all)
  double fadeMs = 4.0;
  double preMs = 3.0;          // slice starts this much before the detected hit, so the attack is kept
};

struct BeatDetectiveResult {
  std::vector<Clip> clips;     // the new playlist
  int hits = 0, moved = 0;
  double meanShiftMs = 0, maxShiftMs = 0;
};

BeatDetectiveResult beatDetective(const std::vector<Clip>& playlist, double sampleRate, const BeatDetectiveParams& p);

}  // namespace wezt::edit
