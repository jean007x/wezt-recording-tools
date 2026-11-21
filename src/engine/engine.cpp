#include "engine/engine.h"

#include <algorithm>
#include <cmath>

#include "engine/denormals.h"

namespace wezt {

Engine::Engine(Graph& graph, std::size_t maxBlockFrames, std::size_t channels, double sampleRate,
               std::size_t scratchCaptureSeconds)
    : graph_(graph),
      block_(channels, maxBlockFrames),
      zeros_(maxBlockFrames, 0.f),
      maxBlock_(maxBlockFrames),
      channels_(channels),
      captureSeconds_(scratchCaptureSeconds),
      captureCapacity_(static_cast<std::size_t>(sampleRate) * scratchCaptureSeconds),
      captureChannels_(channels) {
  devOut_ = static_cast<unsigned>(channels);  // until a device says otherwise
  ctx_.transport = &transport_;
  ctx_.sampleRate = sampleRate;
  ctx_.zeros = zeros_.data();
  capture_.assign(captureChannels_ * captureCapacity_, 0.f);
  inPtrs_.assign(8, nullptr);
  graph_.prepare(sampleRate, channels, maxBlockFrames);
}

void Engine::deviceAboutToStart(const audio::AudioConfig& actual) {
  devIn_ = actual.inChannels;
  devOut_ = actual.outChannels;
  ctx_.inChannels = devIn_;
  ctx_.recordLatencyFrames = static_cast<std::int64_t>(actual.inputLatencyFrames) + actual.outputLatencyFrames;
  if (inPtrs_.size() < devIn_) inPtrs_.assign(devIn_, nullptr);

  const double rate = actual.sampleRate > 0 ? static_cast<double>(actual.sampleRate) : ctx_.sampleRate;
  if (rate != ctx_.sampleRate) {  // device decides the rate (e.g. CoreAudio keeps the hardware's)
    ctx_.sampleRate = rate;
    rateChanged_ = true;
    graph_.prepare(rate, channels_, maxBlock_);  // safe: the callback has not started yet
    captureCapacity_ = static_cast<std::size_t>(rate) * captureSeconds_;
    capture_.assign(captureChannels_ * captureCapacity_, 0.f);
    recorded_.store(0, std::memory_order_release);
  }
}

void Engine::beginRoll(bool record) {   // control thread, or the audio thread at the end of a count-off
  const std::int64_t pre = preRoll_.load(std::memory_order_relaxed), pos = transport_.position();
  if (record && pre > 0 && pos > 0) {
    transport_.locate(std::max<std::int64_t>(0, pos - pre));
    transport_.play();
    recordAt_.store(pos, std::memory_order_release);   // after play(): the audio thread drops it while stopped
  } else if (record) transport_.record();
  else transport_.play();
}

void Engine::startWithCountOff(bool record) {
  const std::int64_t n = metronome_.countOffFrames(ctx_.sampleRate);
  if (n <= 0) { beginRoll(record); return; }
  countOffRecord_.store(record, std::memory_order_relaxed);
  countOffTotal_.store(n, std::memory_order_relaxed);
  countOffLeft_.store(n, std::memory_order_release);
}

void Engine::process(const float* const* in, float* const* out, unsigned frames) {
  ScopedNoDenormals noDenormals;
  unsigned done = 0;
  while (done < frames) {
    std::size_t chunk = std::min<std::size_t>(frames - done, maxBlock_);
    const std::int64_t start = transport_.beginBlock();
    std::int64_t counting = countOffLeft_.load(std::memory_order_acquire);
    if (counting > 0 && transport_.playing()) { countOffLeft_.store(0, std::memory_order_release); counting = 0; }   // already rolling: no count-off
    const bool playing = transport_.playing();
    if (counting > 0) chunk = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(chunk), counting));
    {   // pre-roll: switch recording on exactly at the punch-in frame
      const std::int64_t at = recordAt_.load(std::memory_order_acquire);
      if (at >= 0) {
        if (!playing) recordAt_.store(-1, std::memory_order_release);
        else if (start >= at) { transport_.record(); recordAt_.store(-1, std::memory_order_release); }
        else if (at - start < static_cast<std::int64_t>(chunk)) chunk = static_cast<std::size_t>(at - start);
      }
    }
    if (playing) {  // end the chunk exactly at the loop end so the wrap is sample-accurate
      const std::int64_t toLoop = transport_.framesUntilLoopEnd();
      if (toLoop < static_cast<std::int64_t>(chunk)) chunk = static_cast<std::size_t>(std::max<std::int64_t>(1, toLoop));
    }

    // Input slices for this chunk.
    for (unsigned c = 0; c < devIn_; ++c) inPtrs_[c] = (in && in[c]) ? in[c] + done : zeros_.data();
    ctx_.chunkStart = start;
    ctx_.frames = chunk;
    ctx_.playing = playing;
    ctx_.recording = playing && transport_.recording();
    ctx_.input = devIn_ ? inPtrs_.data() : nullptr;

    // Scratch capture of raw input (diagnostics / tests).
    if (ctx_.recording && devIn_ > 0) {
      const std::size_t at = recorded_.load(std::memory_order_relaxed);
      const std::size_t n = at < captureCapacity_ ? std::min(chunk, captureCapacity_ - at) : 0;
      const std::size_t chans = std::min<std::size_t>(captureChannels_, devIn_);
      for (std::size_t c = 0; c < chans; ++c)
        std::copy(inPtrs_[c], inPtrs_[c] + n, capture_.begin() + static_cast<std::ptrdiff_t>(c * captureCapacity_ + at));
      recorded_.store(at + n, std::memory_order_release);
    }

    graph_.render(block_, chunk);

    // Deliver to the device: copy the channels both sides have, silence any extra device channels.
    for (unsigned c = 0; c < devOut_; ++c) {
      float* dst = out[c] + done;
      if (c < channels_) {  // never hand NaN/Inf (a misbehaving plugin) to the hardware
        const float* src = block_.channel(c);
        for (std::size_t i = 0; i < chunk; ++i) dst[i] = std::isfinite(src[i]) ? src[i] : 0.f;
      }
      else std::fill(dst, dst + chunk, 0.f);
    }
    if (counting > 0) {   // the count-off: only the click is heard, the song waits
      float* mixOut[16]; std::size_t nm = 0;
      for (unsigned c = 0; c < devOut_ && c < 16; ++c) mixOut[nm++] = out[c] + done;
      metronome_.mix(mixOut, nm, chunk, countOffTotal_.load(std::memory_order_relaxed) - counting, ctx_.sampleRate);
      const std::int64_t left = counting - static_cast<std::int64_t>(chunk);
      std::int64_t expected = counting;
      if (countOffLeft_.compare_exchange_strong(expected, left, std::memory_order_acq_rel) && left <= 0) {   // (cancelled meanwhile: leave it)
        beginRoll(countOffRecord_.load(std::memory_order_relaxed));
      }
    } else if (playing && metronome_.enabled()) {
      float* mixOut[16]; std::size_t nm = 0;
      for (unsigned c = 0; c < devOut_ && c < 16; ++c) mixOut[nm++] = out[c] + done;
      metronome_.mix(mixOut, nm, chunk, start, ctx_.sampleRate);
    }
    if (playing) transport_.advance(static_cast<std::int64_t>(chunk));
    done += static_cast<unsigned>(chunk);
  }
}

AudioBuffer Engine::takeRecording() const {
  const std::size_t n = recorded_.load(std::memory_order_acquire);
  AudioBuffer b(captureChannels_, n);
  for (std::size_t c = 0; c < captureChannels_; ++c)
    std::copy(capture_.begin() + static_cast<std::ptrdiff_t>(c * captureCapacity_),
              capture_.begin() + static_cast<std::ptrdiff_t>(c * captureCapacity_ + n), b.channel(c));
  return b;
}

}  // namespace wezt
