#pragma once
// Writing automation while the transport plays, in the usual modes:
//   Off        the lane is ignored
//   Read       the lane plays back
//   Touch      writes while the control is held; when released the value returns to the old automation
//   Latch      starts writing at the first touch and keeps writing the last value until playback stops
//   TouchLatch like Latch, but a fader behaves like Touch
//   Write      writes continuously from the moment playback starts until it stops
//
// The writer never touches the lane while audio runs (the audio thread reads it). It collects the
// points and merges them in commit(), after the transport has stopped.
#include <cstdint>
#include <vector>

#include "session/strip.h"

namespace wezt::session {

enum class AutomationMode { Off, Read, Touch, Latch, TouchLatch, Write };

inline const char* automationModeName(AutomationMode m) {
  switch (m) {
    case AutomationMode::Off: return "off";
    case AutomationMode::Read: return "read";
    case AutomationMode::Touch: return "touch";
    case AutomationMode::Latch: return "latch";
    case AutomationMode::TouchLatch: return "touch/latch";
    case AutomationMode::Write: return "write";
  }
  return "off";
}

class AutomationWriter {
 public:
  void setMode(AutomationMode m) { mode_ = m; }
  AutomationMode mode() const { return mode_; }
  // A fader behaves differently in Touch/Latch.
  void setIsFader(bool f) { fader_ = f; }

  // Call from the control thread while playing, with the timeline position, the control's value and
  // whether the user is holding it. Returns true when the strip must use the control's own value
  // instead of the lane (so the user hears what they are doing).
  bool update(std::int64_t frame, float value, bool touching);
  // Playback stopped or jumped: closes the open segment.
  void endPass(std::int64_t frame);
  bool hasPending() const { return !segs_.empty() || open_; }
  void discard() { segs_.clear(); open_ = false; latched_ = false; }
  // Merges what was recorded into the lane. `returnFrames`: after a Touch release the old
  // automation is rejoined over this many frames. Returns the number of points added.
  std::size_t commit(AutomationLane& lane, std::int64_t returnFrames = 0);

 private:
  struct Seg {
    std::int64_t start = 0, end = 0;
    std::vector<AutomationPoint> pts;
    bool returnToOld = false;
  };
  AutomationMode mode_ = AutomationMode::Read;
  bool fader_ = true, open_ = false, latched_ = false;
  Seg cur_;
  std::vector<Seg> segs_;
};

inline bool AutomationWriter::update(std::int64_t frame, float value, bool touching) {
  const bool writes = mode_ == AutomationMode::Touch || mode_ == AutomationMode::Latch || mode_ == AutomationMode::TouchLatch || mode_ == AutomationMode::Write;
  if (!writes) return false;
  const bool latchLike = mode_ == AutomationMode::Latch || (mode_ == AutomationMode::TouchLatch && !fader_);
  bool active = false;
  switch (mode_) {
    case AutomationMode::Write: active = true; break;
    case AutomationMode::Touch: active = touching; break;
    default:
      if (touching) latched_ = true;
      active = latchLike ? latched_ : touching;
      if (mode_ == AutomationMode::TouchLatch && fader_) active = touching;
      break;
  }
  if (active) {
    if (!open_) { cur_ = Seg(); cur_.start = frame; open_ = true; }
    cur_.end = frame;
    if (cur_.pts.empty() || cur_.pts.back().frame != frame) cur_.pts.push_back({frame, value});
    else cur_.pts.back().value = value;
  } else if (open_) {  // released: hold the last value up to this point
    if (cur_.pts.back().frame != frame) cur_.pts.push_back({frame, cur_.pts.back().value});
    cur_.end = frame;
    cur_.returnToOld = !latchLike && mode_ != AutomationMode::Write;
    segs_.push_back(std::move(cur_));
    open_ = false;
  }
  return active || (latchLike && latched_);
}

inline void AutomationWriter::endPass(std::int64_t frame) {
  if (open_) {
    cur_.end = std::max(cur_.end, frame);
    cur_.returnToOld = false;  // stopped while holding: nothing to return to
    segs_.push_back(std::move(cur_));
    open_ = false;
  }
  latched_ = false;
}

inline std::size_t AutomationWriter::commit(AutomationLane& lane, std::int64_t returnFrames) {
  if (open_) endPass(cur_.end);
  const AutomationLane before = lane;  // the old automation, for the return ramp
  std::size_t added = 0;
  for (auto& s : segs_) {
    if (s.pts.empty()) continue;
    const std::int64_t tail = s.returnToOld && !before.empty() ? returnFrames : 0;
    lane.eraseRange(s.start, s.end + tail);
    for (auto& p : s.pts) { lane.add(p.frame, p.value); ++added; }
    if (tail > 0) { lane.add(s.end + tail, before.valueAt(s.end + tail, s.pts.back().value)); ++added; }
  }
  segs_.clear();
  return added;
}

}  // namespace wezt::session
