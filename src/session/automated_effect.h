// Wraps an effect plug-in so its automation moves inside the block instead of once per block.
// Each audio block is cut where an automation lane has a breakpoint, and at most every
// kMaxSegment frames while a lane is moving; every piece gets the parameter value for its own middle.
// Instruments are wrapped too: the track's MIDI for the block is cut up with it, so every note still starts on its own frame.
#pragma once
#include <cstddef>
#include <memory>

#include "engine/audio_buffer.h"
#include "engine/graph.h"
#include "plugins/plugin_host.h"

namespace wezt::session {

class TrackSource;

class AutomatedEffect final : public Processor {
 public:
  AutomatedEffect(std::unique_ptr<plugins::IPluginInstance> inner, TrackSource* source);
  plugins::IPluginInstance* inner() const { return inner_.get(); }
  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;
  std::size_t latencySamples() const override { return inner_->latencySamples(); }
  static constexpr std::size_t kMaxSegment = 32;
  static constexpr std::size_t kMaxBoundaries = 512;
  static constexpr std::size_t kMaxPieceMidi = 256;
 private:
  std::unique_ptr<plugins::IPluginInstance> inner_;
  TrackSource* source_;
  AudioBuffer scratchIn_, scratchOut_;   // kScratchChannels channels each, sized in prepare()
  static constexpr std::size_t kScratchChannels = 8;
};

}  // namespace wezt::session
