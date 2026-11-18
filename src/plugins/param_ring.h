#pragma once
// Single-producer / single-consumer ring for parameter edits that travel from the UI thread to
// the audio thread without locks or allocation on the audio side. Several control threads (the UI and an
// AI agent, say) may push at once: they take a small mutex among themselves; the audio thread never does.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace wezt::plugins {

template <std::size_t N = 512>
class ParamRing {
 public:
  struct Item {
    std::uint32_t index;  // caller-defined (e.g. position in the plugin's parameter table)
    double value;
  };
  bool push(Item it) {  // any control thread
    std::lock_guard<std::mutex> lock(producers_);
    const auto h = head_.load(std::memory_order_relaxed);
    const auto n = (h + 1) % N;
    if (n == tail_.load(std::memory_order_acquire)) return false;  // full: edit dropped
    buf_[h] = it;
    head_.store(n, std::memory_order_release);
    return true;
  }
  bool tryPush(Item it) {  // the audio thread (automation): gives up instead of waiting for a control thread
    std::unique_lock<std::mutex> lock(producers_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const auto h = head_.load(std::memory_order_relaxed);
    const auto n = (h + 1) % N;
    if (n == tail_.load(std::memory_order_acquire)) return false;
    buf_[h] = it;
    head_.store(n, std::memory_order_release);
    return true;
  }
  bool pop(Item& it) {  // audio thread
    const auto t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    it = buf_[t];
    tail_.store((t + 1) % N, std::memory_order_release);
    return true;
  }

 private:
  std::mutex producers_;  // serializes control threads only
  std::array<Item, N> buf_{};
  std::atomic<std::size_t> head_{0}, tail_{0};
};

}  // namespace wezt::plugins
