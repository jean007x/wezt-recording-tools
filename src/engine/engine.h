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
  // Pre-roll: recording starts from the playhead, but the song rolls in from this many frames earlier (never before 0) so the player
  // hears the lead-in. Recording switches on exactly at the playhead the start was requested at. 0 = off.
  void setPreRollFrames(std::int64_t f) { preRoll_.store(f < 0 ? 0 : f, std::memory_order_relaxed); }
  std::int64_t preRollFrames() const { return preRoll_.load(std::memory_order_relaxed); }
  // Punch recording: roll in (pre-roll), record exactly [startFrame, endFrame), punch out, and keep playing for the post-roll
  // (the same length as the pre-roll) before stopping. Needs a stopped or playing transport; control thread.
  void startPunch(std::int64_t startFrame, std::int64_t endFrame);
  // Loop recording: loops [startFrame, endFrame) and records through every pass; each pass becomes its own take. Rolls in first
  // when a pre-roll is set. Control thread.
  void startLoopRecord(std::int64_t startFrame, std::int64_t endFrame);
  // Plays the song at half speed (an octave lower), starting from the playhead; it lasts until the next stop. Audio and MIDI follow the
  // slower clock; automation lines and plug-in tempo stay approximate. Not while recording.
  void playHalfSpeed();
  bool preRolling() const { return recordAt_.load(std::memory_order_acquire) >= 0; }
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
  std::atomic<bool> halfSpeed_{false};
  unsigned halfPhase_ = 0;   // audio thread
  std::atomic<std::int64_t> preRoll_{0}, recordAt_{-1}, recordEnd_{-1}, stopAt_{-1};
  void beginRoll(bool record);
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
