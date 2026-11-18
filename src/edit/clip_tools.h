#pragma once
// Editing operations on clips that need no UI: strip silence, tab to transients, normalise, edit
// modes (shuffle), separate and consolidate. They work on plain Clip values, so they are easy to
// test and to wrap in undo steps.
#include <cstdint>
#include <vector>

#include <optional>

#include "dsp/elastic.h"
#include "session/track_source.h"

namespace wezt::edit {

using session::Clip;

struct StripSilenceParams {
  float thresholdDb = -50.f;     // below this the clip counts as silent
  double minSilenceMs = 100.0;   // silences shorter than this are kept
  double minClipMs = 50.0;       // pieces shorter than this are dropped
  double padStartMs = 5.0;       // kept before each piece
  double padEndMs = 20.0;        // kept after each piece
  double fadeMs = 3.0;           // fade in/out given to each piece to avoid clicks
};

// Splits a clip into the loud pieces. Pieces keep their timeline position (gaps are left, as in
// Slip mode). Returns {} when the whole clip is silent. Clip ids are 0 for the caller to assign.
std::vector<Clip> stripSilence(const Clip& c, double sampleRate, const StripSilenceParams& p = {});

// Positions (timeline frames) of detected hits inside the clips of a track, sorted. `sensitivity` 0..1.
std::vector<std::int64_t> transientPositions(const std::vector<Clip>& clips, double sampleRate, float sensitivity = 0.5f);
// Next / previous hit after / before `frame`; -1 when there is none.
std::int64_t tabToNextTransient(const std::vector<std::int64_t>& hits, std::int64_t frame);
std::int64_t tabToPreviousTransient(const std::vector<std::int64_t>& hits, std::int64_t frame);

// Peak of the clip's audible part (before clip gain), 0 when silent or missing.
float clipPeak(const Clip& c);
// Gain in dB that brings the clip's peak to `targetDb` (default -1 dBFS). 0 for silent clips.
float normalizeGainDb(const Clip& c, float targetDb = -1.f);

// Splits at a timeline position. Fades stay on the outer edges. False if the position is not inside.
bool separateAt(const Clip& c, std::int64_t frame, Clip& left, Clip& right);

// Shuffle mode: closes the gaps so clips follow each other, in timeline order, starting at the
// earliest clip's start. Overlaps are removed too (the clip order is kept).
std::vector<Clip> shuffleClose(std::vector<Clip> clips);

// Moves a clip by `delta` frames, clamped so it does not start before 0 and, in Shuffle mode, the
// other clips follow. Locked clips do not move. Returns the new playlist.
std::vector<Clip> moveClip(std::vector<Clip> clips, int clipId, std::int64_t delta, bool shuffle);

// Snaps a position to a grid (frames per step). Relative mode keeps the offset to the grid.
std::int64_t snapToGrid(std::int64_t frame, std::int64_t gridFrames);

// Renders a clip with its gain, envelope and fades to a new buffer (consolidate / commit one clip).
std::shared_ptr<AudioBuffer> renderClip(const Clip& c);

// Elastic Audio on one clip: renders it, stretches it by `ratio` (new length / old length, pitch
// unchanged) and returns a new clip at the same start, with the fades scaled. nullopt if the clip
// has no audio or the ratio is outside 0.05..20.
std::optional<Clip> elasticClip(const Clip& c, double ratio, dsp::ElasticMode mode, double sampleRate);
// Same, shifting the pitch by `semitones` and keeping the length.
std::optional<Clip> elasticPitchClip(const Clip& c, double semitones, dsp::ElasticMode mode, double sampleRate);

}  // namespace wezt::edit
