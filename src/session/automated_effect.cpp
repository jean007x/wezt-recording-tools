#include "session/automated_effect.h"

#include <algorithm>
#include <cstring>

#include "session/track_source.h"

namespace wezt::session {

AutomatedEffect::AutomatedEffect(std::unique_ptr<plugins::IPluginInstance> inner, TrackSource* source) : inner_(std::move(inner)), source_(source) {}

void AutomatedEffect::prepare(double sr, std::size_t maxFrames) {
  inner_->prepare(sr, maxFrames);
  scratchIn_ = AudioBuffer(kScratchChannels, std::max<std::size_t>(maxFrames, 1));
  scratchOut_ = AudioBuffer(kScratchChannels, std::max<std::size_t>(maxFrames, 1));
}

void AutomatedEffect::process(const AudioBuffer& in, AudioBuffer& out) {
  std::size_t bounds[kMaxBoundaries];
  std::int64_t chunkStart = 0;
  const std::size_t n = source_ ? source_->autoBoundariesFor(inner_.get(), out.frames, bounds, kMaxBoundaries, chunkStart) : 0;
  const bool fits = in.channels <= kScratchChannels && out.channels <= kScratchChannels && out.frames <= scratchIn_.capacity() && in.frames == out.frames;
  const bool midi = inner_->acceptsMidi();
  std::size_t midiN = 0;
  const MidiEvent* midiAll = source_ && midi ? source_->blockMidi(midiN) : nullptr;
  if (n < 3 || !fits) {   // zero or one segment: the whole block, with the value from its middle
    if (source_) source_->autoApplyFor(inner_.get(), chunkStart + static_cast<std::int64_t>(out.frames / 2));
    if (midiN) inner_->handleMidi(midiAll, midiN);
    inner_->process(in, out);
    return;
  }
  for (std::size_t k = 0; k + 1 < n; ++k) {
    const std::size_t a = bounds[k], b = bounds[k + 1], len = b - a;
    if (len == 0) continue;
    source_->autoApplyFor(inner_.get(), chunkStart + static_cast<std::int64_t>(a + len / 2));
    scratchIn_.channels = in.channels; scratchIn_.frames = len;
    scratchOut_.channels = out.channels; scratchOut_.frames = len;
    for (std::size_t c = 0; c < in.channels; ++c) std::memcpy(scratchIn_.channel(c), in.channel(c) + a, len * sizeof(float));
    if (midiN) {   // this piece's notes, with their offsets counted from the start of the piece
      MidiEvent piece[kMaxPieceMidi];
      std::size_t pn = 0;
      for (std::size_t i = 0; i < midiN && pn < kMaxPieceMidi; ++i) {
        const std::size_t o = midiAll[i].offset;
        if (o < a || (o >= b && b != out.frames)) continue;
        if (o >= out.frames && b != out.frames) continue;
        MidiEvent e = midiAll[i];
        e.offset = static_cast<std::uint32_t>(std::min(o, b - 1) - a);
        piece[pn++] = e;
      }
      if (pn) inner_->handleMidi(piece, pn);
    }
    inner_->process(scratchIn_, scratchOut_);
    for (std::size_t c = 0; c < out.channels; ++c) std::memcpy(out.channel(c) + a, scratchOut_.channel(c), len * sizeof(float));
  }
}

}  // namespace wezt::session
