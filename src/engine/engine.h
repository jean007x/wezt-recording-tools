#pragma once
#include <atomic>
#include <vector>

#include "audio/audio_device.h"
#include "engine/context.h"
#include "engine/graph.h"
#include "engine/metronome.h"
#include "engine/transport.h"

namespace wezt {

// Drives a Graph from a hardware callback.
//
//  * Any callback size works: it is cut into chunks of at most `maxBlockFrames`, and a chunk is
//    also ended exactly at the loop end, so loops wrap sample-accurately.
//  * The graph always runs, even when stopped, so input monitoring, meters and plugin tails work.
//  * If the device runs at another sample rate or has fewer/more channels than the engine was built
//    for, the graph is re-prepared at the device's rate and the extra/missing channels are handled
//    (never read or written out of range).
//  * Denormals are flushed to zero on the audio thread.
//
// Threading: configure (graph edits) only while the device is stopped. Transport may be used from
// any thread while the device runs.
class Engine : public audio::AudioCallback {
 public:
  Engine(Graph& graph, std::size_t maxBlockFrames, std::size_t channels, double sampleRate,
         std::size_t scratchCaptureSeconds = 60);

  Transport& transport() { return transport_; }
  Metronome& metronome() { return metronome_; }
  // Starts the transport (recording or just playing) after the metronome's count-off, if one is set; with none it starts at once.
  // While counting, the playhead stays where it is and nothing plays but the click. cancelCountOff() stops that without starting.
  void startWithCountOff(bool record);
  void cancelCountOff() { countOffLeft_.store(0, std::memory_order_release); }
  bool countingOff() const { return countOffLeft_.load(std::memory_order_acquire) > 0; }
  // Processors (e.g. the mixer) keep this pointer and read it inside process().
  const RtContext* context() const { return &ctx_; }

  double sampleRate() const { return ctx_.sampleRate; }
  // True if the device ran at a different rate than the engine was created with (graph was re-prepared).
  bool sampleRateChanged() const { return rateChanged_; }

  // Simple RAM capture of raw input channels while the transport records. Meant for diagnostics and
  // tests; real recording goes to disk (see DiskRecorder).
  std::size_t recordedFrames() const { return recorded_.load(std::memory_order_acquire); }
  AudioBuffer takeRecording() const;  // call after stop()
  void clearRecording() { recorded_.store(0, std::memory_order_release); }

  void deviceAboutToStart(const audio::AudioConfig& actual) override;
  void process(const float* const* in, float* const* out, unsigned frames) override;

 private:
  Graph& graph_;
  Transport transport_;
  Metronome metronome_;
  std::atomic<std::int64_t> countOffLeft_{0}, countOffTotal_{0};
  std::atomic<bool> countOffRecord_{false};
  RtContext ctx_;
  AudioBuffer block_;
  std::vector<float> zeros_;
  std::size_t maxBlock_, channels_;
  unsigned devIn_ = 0, devOut_ = 0;
  bool rateChanged_ = false;
  std::vector<float> capture_;  // inChannels * captureCapacity_
  std::size_t captureSeconds_ = 0, captureCapacity_ = 0, captureChannels_ = 0;
  std::atomic<std::size_t> recorded_{0};
  std::vector<const float*> inPtrs_;  // per-chunk input slice pointers
};

}  // namespace wezt
