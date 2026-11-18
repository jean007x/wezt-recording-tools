#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace wezt {

// Lock-free transport state. The UI thread writes commands; the audio thread reads state and
// advances the position. All members are safe to touch from either thread.
class Transport {
 public:
  void play() { playing_.store(true, std::memory_order_release); }
  void stop() {
    recording_.store(false, std::memory_order_release);
    playing_.store(false, std::memory_order_release);
  }
  void record() {  // roll with recording; recording is only ever true while playing
    playing_.store(true, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
  }
  void locate(std::int64_t frame) { locateRequest_.store(frame < 0 ? 0 : frame, std::memory_order_release); }
  void setLoop(std::int64_t startFrame, std::int64_t endFrame) {
    // Stop honouring the old loop while the pair is being replaced, so the audio thread never sees
    // a start from one loop and an end from another.
    looping_.store(false, std::memory_order_release);
    loopStart_.store(startFrame, std::memory_order_relaxed);
    loopEnd_.store(endFrame, std::memory_order_relaxed);
    looping_.store(endFrame > startFrame && startFrame >= 0, std::memory_order_release);
  }
  void clearLoop() { looping_.store(false, std::memory_order_release); }

  bool playing() const { return playing_.load(std::memory_order_acquire); }
  bool recording() const { return recording_.load(std::memory_order_acquire); }
  bool looping() const { return looping_.load(std::memory_order_acquire); }
  std::int64_t position() const { return position_.load(std::memory_order_acquire); }

  // --- audio thread only ---
  // Applies a pending locate request; returns the frame at which the next chunk starts.
  std::int64_t beginBlock() {
    std::int64_t req = locateRequest_.exchange(-1, std::memory_order_acq_rel);
    if (req >= 0) position_.store(req, std::memory_order_release);
    return position_.load(std::memory_order_acquire);
  }
  // How many frames can be rendered before the loop end is reached, so the engine can end a chunk
  // exactly there and wrap sample-accurately. Unlimited when not looping or already past the end.
  std::int64_t framesUntilLoopEnd() const {
    if (!looping_.load(std::memory_order_acquire)) return std::numeric_limits<std::int64_t>::max();
    const std::int64_t e = loopEnd_.load(std::memory_order_relaxed);
    const std::int64_t p = position_.load(std::memory_order_relaxed);
    return p < e ? e - p : std::numeric_limits<std::int64_t>::max();
  }
  // Advances by one chunk, wrapping at the loop end if looping.
  void advance(std::int64_t frames) {
    std::int64_t p = position_.load(std::memory_order_relaxed) + frames;
    if (looping_.load(std::memory_order_acquire)) {
      const std::int64_t s = loopStart_.load(std::memory_order_relaxed);
      const std::int64_t e = loopEnd_.load(std::memory_order_relaxed);
      if (e > s && p >= e) p = s + ((p - s) % (e - s));
    }
    position_.store(p, std::memory_order_release);
  }

 private:
  std::atomic<bool> playing_{false}, recording_{false}, looping_{false};
  std::atomic<std::int64_t> position_{0}, locateRequest_{-1}, loopStart_{0}, loopEnd_{0};
};

}  // namespace wezt
