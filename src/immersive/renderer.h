#pragma once
// Object-based renderer: beds and objects in, speaker feeds out.
//
// Object positions use a unit room: x -1 (left) .. +1 (right), y -1 (back) .. +1 (front), z 0 (ear
// level) .. 1 (ceiling); the listener sits at (0,0,0). Panning is VBAP (vector base amplitude
// panning) over triangles of the layout, constant power. An object near the centre of the room, or
// with a large size, spreads over all speakers. This is our own renderer, not a Dolby product.
#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "engine/graph.h"
#include "immersive/layout.h"
#include "session/strip.h"

namespace wezt::immersive {

// Gains for a direction over a layout.
class Vbap {
 public:
  explicit Vbap(const SpeakerLayout& layout);
  // Unit direction (x right, y front, z up) -> one gain per layout speaker (0 for the LFE). Sum of squares is 1.
  void gains(float x, float y, float z, float* out) const;
  std::size_t triangleCount() const { return tris_.size(); }

 private:
  struct Tri { int a, b, c; float inv[9]; };
  bool solve(const Tri& t, const float p[3], float g[3]) const;
  SpeakerLayout layout_;
  std::vector<int> main_;                  // layout indices of non-LFE speakers
  std::vector<std::array<float, 3>> dir_;  // directions of main_ (+ a virtual nadir speaker when there are heights)
  std::vector<Tri> tris_;
  bool horizontalOnly_ = true;
  std::vector<std::pair<int, int>> pairs_;  // horizontal layouts: neighbouring speakers (indices into main_)
  std::vector<float> pairInv_;              // 4 floats per pair
};

constexpr std::size_t kMaxSpeakers = 24;

struct ObjectState {
  std::atomic<float> x{0.f}, y{1.f}, z{0.f}, size{0.f}, gainDb{0.f}, lfeSend{0.f};
  std::atomic<bool> mute{false};
  session::AutomationLane laneX, laneY, laneZ;  // follow the timeline while playing when automation is on (set up while stopped)
};

class ObjectRenderer : public Processor {
 public:
  // The first `bed.size()` input channels are bed channels (each fixed at its speaker's direction),
  // followed by `objects` object channels. Output channels = output layout size.
  ObjectRenderer(const SpeakerLayout& output, const SpeakerLayout& bed, std::size_t objects);

  void setContext(const RtContext* c) { ctx_ = c; }
  void setAutomationOn(bool on) { automate_.store(on, std::memory_order_relaxed); }

  std::size_t bedChannels() const { return bed_.size(); }
  std::size_t objectCount() const { return objects_.size(); }
  std::size_t inputChannels() const { return bed_.size() + objects_.size(); }
  const SpeakerLayout& outputLayout() const { return out_; }
  ObjectState& object(std::size_t i) { return *objects_[i]; }
  void setBedGainDb(float db) { bedGainDb_.store(db, std::memory_order_relaxed); }

  // Speaker gains for a position, for display (UI thread).
  std::vector<float> gainsFor(float x, float y, float z, float size) const;
  // Peak of each output speaker since the last call.
  float takePeak(std::size_t speaker) { return speaker < kMaxSpeakers ? peaks_[speaker].exchange(0.f, std::memory_order_relaxed) : 0.f; }

  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;

 private:
  void targetGains(std::size_t obj, std::int64_t frame, float* g) const;
  SpeakerLayout out_, bed_;
  Vbap vbap_;
  std::vector<std::unique_ptr<ObjectState>> objects_;
  std::vector<std::array<float, kMaxSpeakers>> cur_;  // current gains per input channel (bed first)
  std::vector<std::array<float, kMaxSpeakers>> bedGains_;
  std::atomic<float> bedGainDb_{0.f};
  std::atomic<bool> automate_{true};
  const RtContext* ctx_ = nullptr;
  std::array<std::atomic<float>, kMaxSpeakers> peaks_{};
  bool first_ = true;
};

}  // namespace wezt::immersive
