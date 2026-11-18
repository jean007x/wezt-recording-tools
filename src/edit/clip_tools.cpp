#include "edit/clip_tools.h"

#include <algorithm>
#include <cmath>

#include "instruments/sampler.h"

namespace wezt::edit {
namespace {

// Clip-relative gain line for the piece that starts `pos` frames into the original and lasts `len`.
session::AutomationLane shiftEnv(const session::AutomationLane& e, std::int64_t pos, std::int64_t len) {
  session::AutomationLane out;
  if (e.empty()) return out;
  out.add(0, e.valueAt(pos, 0.f));
  for (auto& p : e.points())
    if (p.frame > pos && p.frame < pos + len) out.add(p.frame - pos, p.value);
  out.add(std::max<std::int64_t>(1, len), e.valueAt(pos + len, 0.f));
  return out;
}

bool audible(const Clip& c) { return c.audio && c.audio->frames > 0 && c.length > 0; }

}  // namespace

std::vector<Clip> stripSilence(const Clip& c, double sr, const StripSilenceParams& p) {
  std::vector<Clip> out;
  if (!audible(c) || sr <= 0) return out;
  const std::int64_t avail = static_cast<std::int64_t>(c.audio->frames);
  const std::int64_t len = std::min(c.length, std::max<std::int64_t>(0, avail - c.offset));
  if (len <= 0) return out;
  const std::int64_t hop = std::max<std::int64_t>(1, static_cast<std::int64_t>(sr * 0.001));
  const float thr = session::dbToLin(p.thresholdDb);
  const std::int64_t nh = (len + hop - 1) / hop;
  std::vector<char> loud(static_cast<std::size_t>(nh), 0);
  for (std::int64_t h = 0; h < nh; ++h) {
    float pk = 0.f;
    const std::int64_t a = c.offset + h * hop, b = std::min(c.offset + len, a + hop);
    for (std::size_t ch = 0; ch < c.audio->channels && pk <= thr; ++ch) {
      const float* x = c.audio->channel(ch);
      for (std::int64_t i = a; i < b; ++i) pk = std::max(pk, std::fabs(x[i]));
    }
    loud[static_cast<std::size_t>(h)] = pk > thr;
  }
  struct Run { std::int64_t a, b; };  // in hops, [a,b)
  std::vector<Run> runs;
  for (std::int64_t h = 0; h < nh; ++h)
    if (loud[static_cast<std::size_t>(h)]) {
      if (!runs.empty() && runs.back().b == h) ++runs.back().b; else runs.push_back({h, h + 1});
    }
  const std::int64_t minSil = std::max<std::int64_t>(1, static_cast<std::int64_t>(p.minSilenceMs / 1000.0 * sr / static_cast<double>(hop)));
  std::vector<Run> merged;
  for (auto& r : runs) {
    if (!merged.empty() && r.a - merged.back().b < minSil) merged.back().b = r.b; else merged.push_back(r);
  }
  const std::int64_t minClip = static_cast<std::int64_t>(p.minClipMs / 1000.0 * sr);
  const std::int64_t padS = static_cast<std::int64_t>(p.padStartMs / 1000.0 * sr), padE = static_cast<std::int64_t>(p.padEndMs / 1000.0 * sr);
  const std::int64_t fade = static_cast<std::int64_t>(p.fadeMs / 1000.0 * sr);
  std::int64_t prevEnd = 0;
  for (auto& r : merged) {
    std::int64_t a = std::max(prevEnd, r.a * hop - padS), b = std::min(len, r.b * hop + padE);
    if (b - a < minClip || b <= a) continue;
    Clip n = c;
    n.id = 0;
    n.start = c.start + a;
    n.offset = c.offset + a;
    n.length = b - a;
    n.fadeIn = a == 0 ? c.fadeIn : std::min(fade, n.length / 2);
    n.fadeOut = b == len ? c.fadeOut : std::min(fade, n.length / 2);
    n.gainEnv = shiftEnv(c.gainEnv, a, n.length);
    out.push_back(std::move(n));
    prevEnd = b;
  }
  return out;
}

std::vector<std::int64_t> transientPositions(const std::vector<Clip>& clips, double sr, float sensitivity) {
  std::vector<std::int64_t> hits;
  for (auto& c : clips) {
    if (!audible(c)) continue;
    // Analyse only the part of the file the clip uses.
    const std::int64_t avail = static_cast<std::int64_t>(c.audio->frames);
    const std::int64_t from = std::clamp<std::int64_t>(c.offset, 0, avail), to = std::clamp<std::int64_t>(c.offset + c.length, from, avail);
    if (to - from < 16) continue;
    AudioBuffer part(c.audio->channels, static_cast<std::size_t>(to - from));
    for (std::size_t ch = 0; ch < part.channels; ++ch) std::copy(c.audio->channel(ch) + from, c.audio->channel(ch) + to, part.channel(ch));
    for (std::int64_t t : instruments::Sampler::findTransients(part, sr, sensitivity, 4096))
      if (t > 0) hits.push_back(c.start + t);  // the finder always reports the start of the audio; that is not a hit
  }
  std::sort(hits.begin(), hits.end());
  hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
  return hits;
}
std::int64_t tabToNextTransient(const std::vector<std::int64_t>& hits, std::int64_t frame) {
  auto it = std::upper_bound(hits.begin(), hits.end(), frame);
  return it == hits.end() ? -1 : *it;
}
std::int64_t tabToPreviousTransient(const std::vector<std::int64_t>& hits, std::int64_t frame) {
  auto it = std::lower_bound(hits.begin(), hits.end(), frame);
  return it == hits.begin() ? -1 : *(it - 1);
}

float clipPeak(const Clip& c) {
  if (!audible(c)) return 0.f;
  const std::int64_t avail = static_cast<std::int64_t>(c.audio->frames);
  const std::int64_t from = std::clamp<std::int64_t>(c.offset, 0, avail), to = std::clamp<std::int64_t>(c.offset + c.length, from, avail);
  float pk = 0.f;
  for (std::size_t ch = 0; ch < c.audio->channels; ++ch) {
    const float* x = c.audio->channel(ch);
    for (std::int64_t i = from; i < to; ++i) pk = std::max(pk, std::fabs(x[i]));
  }
  return pk;
}
float normalizeGainDb(const Clip& c, float targetDb) {
  const float pk = clipPeak(c);
  return pk <= 1e-6f ? 0.f : targetDb - session::linToDb(pk);
}

bool separateAt(const Clip& c, std::int64_t frame, Clip& left, Clip& right) {
  if (frame <= c.start || frame >= c.start + c.length || c.locked) return false;
  const std::int64_t cut = frame - c.start;
  left = c; right = c;
  left.length = cut;
  left.fadeOut = 0;
  left.gainEnv = shiftEnv(c.gainEnv, 0, cut);
  right.start = frame;
  right.offset = c.offset + cut;
  right.length = c.length - cut;
  right.fadeIn = 0;
  right.gainEnv = shiftEnv(c.gainEnv, cut, right.length);
  left.fadeIn = std::min(left.fadeIn, left.length);
  right.fadeOut = std::min(right.fadeOut, right.length);
  right.id = 0;
  return true;
}

std::vector<Clip> shuffleClose(std::vector<Clip> clips) {
  if (clips.empty()) return clips;
  std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
  std::int64_t pos = clips.front().start;
  for (auto& c : clips) { c.start = pos; pos += c.length; }
  return clips;
}

std::vector<Clip> moveClip(std::vector<Clip> clips, int id, std::int64_t delta, bool shuffle) {
  for (auto& c : clips) {
    if (c.id != id) continue;
    if (c.locked) return clips;
    c.start = std::max<std::int64_t>(0, c.start + delta);
    break;
  }
  if (shuffle) {
    // The moved clip keeps its place in the new order; everything else closes up behind it.
    return shuffleClose(std::move(clips));
  }
  return clips;
}

std::int64_t snapToGrid(std::int64_t frame, std::int64_t g) {
  if (g <= 0) return frame;
  const std::int64_t q = (frame + g / 2) / g * g;
  return q < 0 ? 0 : q;
}

std::shared_ptr<AudioBuffer> renderClip(const Clip& c) {
  if (!audible(c)) return nullptr;
  const std::size_t n = static_cast<std::size_t>(c.length);
  auto out = std::make_shared<AudioBuffer>(c.audio->channels, n);
  const std::int64_t avail = static_cast<std::int64_t>(c.audio->frames);
  const float g = session::dbToLin(c.gainDb);
  for (std::int64_t pos = 0; pos < c.length; ++pos) {
    const std::int64_t idx = c.offset + pos;
    if (idx < 0 || idx >= avail) continue;
    float w = 1.f;
    if (c.fadeIn > 0 && pos < c.fadeIn) w = static_cast<float>(pos) / static_cast<float>(c.fadeIn);
    if (c.fadeOut > 0 && c.length - pos <= c.fadeOut) w = std::min(w, static_cast<float>(c.length - pos) / static_cast<float>(c.fadeOut));
    const float e = c.gainEnv.empty() ? 1.f : session::dbToLin(c.gainEnv.valueAt(pos, 0.f));
    for (std::size_t ch = 0; ch < out->channels; ++ch) out->channel(ch)[pos] = c.audio->channel(ch)[idx] * g * e * w;
  }
  return out;
}

std::optional<Clip> elasticClip(const Clip& c, double ratio, dsp::ElasticMode mode, double sr) {
  const auto r = renderClip(c);
  if (!r || r->frames == 0) return std::nullopt;
  AudioBuffer out = dsp::elasticStretch(*r, ratio, mode, sr);
  if (out.frames == 0) return std::nullopt;
  Clip n = c;
  n.audio = std::make_shared<const AudioBuffer>(std::move(out));
  n.offset = 0;
  n.length = static_cast<std::int64_t>(n.audio->frames);
  n.gainDb = 0.f;
  n.gainEnv.clear();
  n.fadeIn = static_cast<std::int64_t>(static_cast<double>(c.fadeIn) * ratio);
  n.fadeOut = static_cast<std::int64_t>(static_cast<double>(c.fadeOut) * ratio);
  n.name = (c.name.empty() ? std::string("clip") : c.name) + " (elastic)";
  return n;
}

std::optional<Clip> elasticPitchClip(const Clip& c, double semitones, dsp::ElasticMode mode, double sr) {
  const auto r = renderClip(c);
  if (!r || r->frames == 0) return std::nullopt;
  AudioBuffer out = dsp::elasticPitch(*r, semitones, mode, sr);
  if (out.frames == 0) return std::nullopt;
  Clip n = c;
  n.audio = std::make_shared<const AudioBuffer>(std::move(out));
  n.offset = 0;
  n.length = static_cast<std::int64_t>(n.audio->frames);
  n.gainDb = 0.f;
  n.gainEnv.clear();
  n.name = (c.name.empty() ? std::string("clip") : c.name) + " (pitch)";
  return n;
}

}  // namespace wezt::edit
