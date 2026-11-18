#pragma once
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "engine/audio_buffer.h"
#include "engine/delay_line.h"

namespace wezt {

// A unit of audio processing. process() gets `in.frames` == `out.frames` valid frames, any value up
// to the maxFrames passed to prepare(). It must not allocate, lock or block.
class Processor {
 public:
  virtual ~Processor() = default;
  virtual void prepare(double sampleRate, std::size_t maxFrames) { (void)sampleRate; (void)maxFrames; }
  virtual void process(const AudioBuffer& in, AudioBuffer& out) = 0;
  // Samples between input and output (e.g. a look-ahead limiter). Read after prepare().
  virtual std::size_t latencySamples() const { return 0; }
};

// Gain with a click-free ramp: set from any thread, applied over the next block.
class Gain : public Processor {
 public:
  explicit Gain(float g) : target_(g), current_(g) {}
  void setGain(float g) { target_.store(g, std::memory_order_relaxed); }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    const float g0 = current_, g1 = target_.load(std::memory_order_relaxed);
    const std::size_t n = out.frames, ch = std::min(in.channels, out.channels);
    for (std::size_t c = 0; c < ch; ++c) {
      const float* x = in.channel(c);
      float* y = out.channel(c);
      if (g0 == g1) {
        for (std::size_t i = 0; i < n; ++i) y[i] = x[i] * g1;
      } else {
        const float step = (g1 - g0) / static_cast<float>(n);
        for (std::size_t i = 0; i < n; ++i) y[i] = x[i] * (g0 + step * static_cast<float>(i + 1));
      }
    }
    for (std::size_t c = ch; c < out.channels; ++c) std::fill(out.channel(c), out.channel(c) + n, 0.f);
    current_ = g1;
  }

 private:
  std::atomic<float> target_;
  float current_;
};

class Sine : public Processor {
 public:
  explicit Sine(double freq) : freq_(freq) {}
  void prepare(double sr, std::size_t) override { sr_ = sr; phase_ = 0; }
  void process(const AudioBuffer&, AudioBuffer& out) override {
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const double inc = kTwoPi * freq_ / sr_;
    for (std::size_t f = 0; f < out.frames; ++f) {
      const float s = static_cast<float>(std::sin(phase_));
      phase_ += inc;
      if (phase_ > kTwoPi) phase_ -= kTwoPi;  // keep precision over long runs
      for (std::size_t c = 0; c < out.channels; ++c) out.channel(c)[f] = s;
    }
  }

 private:
  double freq_, sr_ = 48000, phase_ = 0;
};

// Directed acyclic graph of processors. Nodes run in dependency order; a node's input is the sum
// of its upstream nodes. Latency is compensated automatically: where paths with different delays
// meet, the shorter ones are delayed so everything stays time-aligned.
//
// Editing (add/remove/connect/prepare) allocates and must happen while audio is stopped, or at
// least not concurrently with render().
class Graph {
 public:
  using NodeId = std::size_t;

  NodeId add(std::unique_ptr<Processor> p);   // prepared immediately if the graph already is
  bool remove(NodeId n);                       // ids of other nodes stay valid
  bool connect(NodeId from, NodeId to);        // false if invalid or it would make a cycle; duplicates are ignored
  bool disconnect(NodeId from, NodeId to);
  void setOutput(NodeId n) { output_ = n; }

  void prepare(double sampleRate, std::size_t channels, std::size_t maxFrames);
  // Renders `frames` frames (clamped to the prepared maximum) into `out`. For real-time use pass a
  // buffer already sized channels x maxFrames; otherwise it is reallocated here.
  void render(AudioBuffer& out, std::size_t frames);
  void render(AudioBuffer& out) { render(out, maxFrames_); }

  // Recomputes delay compensation; call after a plugin's reported latency changed (not on the audio thread).
  void refreshLatency();
  std::size_t latencyAt(NodeId n) const;       // samples between the graph's sources and this node's output
  std::size_t outputLatency() const { return output_ < nodes_.size() ? latencyAt(output_) : 0; }

  std::size_t nodeCount() const;               // live nodes
  Processor* processor(NodeId n);
  bool prepared() const { return prepared_; }

  // Fault containment: NaN or infinity in a node's output is replaced by 0 before any other node sees it. After
  // kQuarantineAfter blocks in a row with such samples the node is muted (not run) until resetFault(). Counters are plain
  // values written by the audio thread: read them while the audio is stopped, or accept a slightly stale number.
  static constexpr std::uint32_t kQuarantineAfter = 4;
  std::uint64_t nonFiniteBlocks(NodeId n) const { return alive(n) ? nodes_[n].faultBlocks : 0; }
  bool quarantined(NodeId n) const { return alive(n) && nodes_[n].quarantined; }
  void resetFault(NodeId n) { if (alive(n)) { nodes_[n].badRun = 0; nodes_[n].quarantined = false; } }

 private:
  struct Node {
    std::unique_ptr<Processor> proc;  // null = removed
    std::vector<NodeId> inputs;
    std::vector<DelayLine> edgeDelay;  // parallel to inputs
    AudioBuffer in, out;
    std::size_t pathLatency = 0;
    std::uint64_t faultBlocks = 0;   // blocks that contained NaN/inf
    std::uint32_t badRun = 0;        // consecutive such blocks
    bool quarantined = false;
  };
  bool alive(NodeId n) const { return n < nodes_.size() && nodes_[n].proc != nullptr; }
  bool reaches(NodeId from, NodeId target) const;
  void rebuildOrder();
  void prepareNode(Node& n);

  std::vector<Node> nodes_;
  std::vector<NodeId> order_;
  AudioBuffer scratch_;
  NodeId output_ = 0;
  double sampleRate_ = 48000;
  std::size_t channels_ = 2, maxFrames_ = 0;
  bool prepared_ = false;
};

}  // namespace wezt
