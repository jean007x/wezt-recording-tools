#include "session/track_source.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <filesystem>

#include "session/strip.h"

namespace wezt::session {
namespace fs = std::filesystem;

namespace {
struct SpinGuard {  // UI-side lock: may spin, the audio thread never does
  explicit SpinGuard(std::atomic_flag& f) : f_(f) { while (f_.test_and_set(std::memory_order_acquire)) std::this_thread::yield(); }
  ~SpinGuard() { f_.clear(std::memory_order_release); }
  std::atomic_flag& f_;
};
}  // namespace

TrackSource::TrackSource() = default;

TrackSource::~TrackSource() {
  quit_.store(true);
  if (writer_.joinable()) writer_.join();
}

void TrackSource::setPlaylist(std::vector<Clip> clips) {
  SpinGuard g(lock_);
  clips_ = std::move(clips);
}

std::vector<Clip> TrackSource::playlist() const {
  SpinGuard g(lock_);
  return clips_;
}

void TrackSource::setInputs(std::vector<unsigned> ch) {
  if (ch.empty()) ch.assign(1, 0u);
  if (ch.size() > 2) ch.erase(ch.begin() + 2, ch.end());
  std::lock_guard<std::mutex> l(cfgMutex_);
  inputs_ = std::move(ch);
  recCh_ = inputs_.size();
  if (maxFrames_) {  // re-size the ring for the new channel count
    slotData_.assign(slotCount_ * recCh_ * maxFrames_, 0.f);
    head_.store(0); tail_.store(0);
  }
}

void TrackSource::setRecordFolder(const std::string& dir, const std::string& baseName, io::WavSampleType type) {
  std::lock_guard<std::mutex> l(cfgMutex_);
  dir_ = dir;
  base_ = baseName;
  wavType_ = type;
}

void TrackSource::startWriterIfNeeded() {
  if (!writer_.joinable()) writer_ = std::thread([this] { writerLoop(); });
}

void TrackSource::arm(bool on) {
  if (on) startWriterIfNeeded();
  armed_.store(on, std::memory_order_relaxed);
}

std::string TrackSource::lastError() const {
  std::lock_guard<std::mutex> l(takesMutex_);
  return error_;
}
void TrackSource::setError(const std::string& e) {
  std::lock_guard<std::mutex> l(takesMutex_);
  error_ = e;
}

std::vector<Take> TrackSource::collectTakes() {
  std::lock_guard<std::mutex> l(takesMutex_);
  std::vector<Take> t;
  t.swap(takes_);
  return t;
}

bool TrackSource::waitIdle(int timeoutMs) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < end) {
    if (head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire) && !writerOpen_.load(std::memory_order_acquire)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

void TrackSource::prepare(double sampleRate, std::size_t maxFrames) {
  std::lock_guard<std::mutex> l(cfgMutex_);
  sampleRate_ = sampleRate;
  maxFrames_ = maxFrames;
  slotCount_ = std::max<std::size_t>(32, static_cast<std::size_t>(3.0 * sampleRate / static_cast<double>(std::max<std::size_t>(maxFrames, 1))) + 4);
  slots_.assign(slotCount_, Slot{});
  slotData_.assign(slotCount_ * recCh_ * maxFrames_, 0.f);
  head_.store(0); tail_.store(0);
  recording_ = false;
}

bool TrackSource::pushSlot(int type, std::int64_t pos, std::int64_t skip, const float* const* planar, std::uint32_t frames, bool reserve) {
  const std::size_t h = head_.load(std::memory_order_relaxed), t = tail_.load(std::memory_order_acquire);
  const std::size_t used = (h + slotCount_ - t) % slotCount_;
  if (used + 1 + (reserve ? 2 : 0) >= slotCount_) return false;  // keep room for Start/Stop
  Slot& s = slots_[h];
  s.type = type; s.frames = frames; s.pos = pos; s.skip = skip;
  if (planar && frames) {
    float* d = slotData_.data() + h * recCh_ * maxFrames_;
    for (std::size_t c = 0; c < recCh_; ++c) std::copy(planar[c], planar[c] + frames, d + c * maxFrames_);
  }
  head_.store((h + 1) % slotCount_, std::memory_order_release);
  return true;
}

void TrackSource::renderClips(AudioBuffer& out, std::int64_t chunkStart) {
  const std::int64_t n = static_cast<std::int64_t>(out.frames), chunkEnd = chunkStart + n;
  for (const Clip& c : clips_) {
    if (!c.audio || c.length <= 0 || c.muted) continue;
    const std::int64_t from = std::max(chunkStart, c.start), to = std::min(chunkEnd, c.start + c.length);
    if (from >= to) continue;
    const float gain = dbToLin(c.gainDb);
    const std::int64_t avail = static_cast<std::int64_t>(c.audio->frames);
    for (std::int64_t t = from; t < to; ++t) {
      const std::int64_t pos = t - c.start, idx = c.offset + pos;
      if (idx < 0 || idx >= avail) continue;  // past the end of the file: silence
      float w = 1.f;
      if (c.fadeIn > 0 && pos < c.fadeIn) w = static_cast<float>(pos) / static_cast<float>(c.fadeIn);
      if (c.fadeOut > 0 && c.length - pos <= c.fadeOut) w = std::min(w, static_cast<float>(c.length - pos) / static_cast<float>(c.fadeOut));
      const std::size_t o = static_cast<std::size_t>(t - chunkStart);
      const float envGain = c.gainEnv.empty() ? 1.f : dbToLin(c.gainEnv.valueAt(pos, 0.f));
      for (std::size_t ch = 0; ch < out.channels; ++ch) {
        const std::size_t sc = c.audio->channels == 1 ? 0 : ch;  // mono plays on every channel
        const float s = sc < c.audio->channels ? c.audio->channel(sc)[idx] * gain * envGain : 0.f;
        float& y = out.channel(ch)[o];
        y = y * (1.f - w) + s * w;  // later clips replace earlier ones; fades cross-fade
      }
    }
  }
}

void TrackSource::setMidiSequence(std::shared_ptr<const MidiSequence> seq) {
  while (midiLock_.test_and_set(std::memory_order_acquire)) {}
  seq_ = std::move(seq);
  midiLastEnd_ = -1;  // the next block re-syncs and releases anything still sounding
  midiLock_.clear(std::memory_order_release);
}

std::shared_ptr<const MidiSequence> TrackSource::midiSequence() const {
  while (midiLock_.test_and_set(std::memory_order_acquire)) {}
  auto s = seq_;
  midiLock_.clear(std::memory_order_release);
  return s;
}

void TrackSource::setMidiTargets(std::vector<plugins::IPluginInstance*> targets) {
  targets.erase(std::remove_if(targets.begin(), targets.end(), [](plugins::IPluginInstance* p) { return !p || !p->acceptsMidi(); }), targets.end());
  while (midiLock_.test_and_set(std::memory_order_acquire)) {}
  targets_ = std::move(targets);
  midiLock_.clear(std::memory_order_release);
}

bool TrackSource::auditionPush(const MidiEvent& e) {
  const auto h = audHead_.load(std::memory_order_relaxed);
  const auto n = (h + 1) % aud_.size();
  if (n == audTail_.load(std::memory_order_acquire)) return false;
  aud_[h] = e;
  audHead_.store(n, std::memory_order_release);
  return true;
}

bool TrackSource::auditionNote(int key, int velocity, bool on) {
  if (key < 0 || key > 127) return false;
  MidiEvent e;
  e.type = on ? MidiEvent::NoteOn : MidiEvent::NoteOff;
  e.a = static_cast<std::int16_t>(key);
  e.b = static_cast<std::int16_t>(on ? std::clamp(velocity, 1, 127) : 0);
  return auditionPush(e);
}

bool TrackSource::auditionController(int cc, int value) {
  if (cc < 0 || cc > 127) return false;
  MidiEvent e; e.type = MidiEvent::Controller; e.a = static_cast<std::int16_t>(cc); e.b = static_cast<std::int16_t>(std::clamp(value, 0, 127));
  return auditionPush(e);
}

bool TrackSource::auditionPitchBend(int value14) {
  MidiEvent e; e.type = MidiEvent::PitchBend; e.a = static_cast<std::int16_t>(std::clamp(value14, 0, 16383));
  return auditionPush(e);
}

void TrackSource::auditionAllOff() { audPanic_.store(true, std::memory_order_release); }

void TrackSource::dispatchMidi(std::size_t frames) {
  if (midiLock_.test_and_set(std::memory_order_acquire)) return;  // an edit is in progress: try again next block
  struct Release { std::atomic_flag& f; ~Release() { f.clear(std::memory_order_release); } } release{midiLock_};
  const bool playing = ctx_->playing;
  const std::int64_t start = ctx_->chunkStart, end = start + static_cast<std::int64_t>(frames);
  if (targets_.empty()) {  // nothing to play to; keep the bookkeeping honest and drain the preview queue
    audTail_.store(audHead_.load(std::memory_order_acquire), std::memory_order_release);
    audPanic_.store(false, std::memory_order_relaxed);
    midiWasPlaying_ = playing; midiLastEnd_ = end; midiCursor_ = end;
    return;
  }
  MidiEvent buf[kMaxMidiPerBlock];
  std::size_t n = 0;
  auto add = [&](const MidiEvent& e) { if (n < kMaxMidiPerBlock) buf[n++] = e; else midiDropped_.fetch_add(1, std::memory_order_relaxed); };
  // sequencer
  const bool contiguous = midiWasPlaying_ && start == midiLastEnd_;
  if ((!playing && midiWasPlaying_) || (playing && !contiguous && midiWasPlaying_)) { MidiEvent off; off.type = MidiEvent::AllNotesOff; add(off); }  // stopped, or the playhead jumped
  if (playing && seq_) {
    if (!contiguous) midiCursor_ = start;
    MidiEvent tmp[kMaxMidiPerBlock];
    unsigned dropped = 0;
    const std::size_t got = collectMidi(*seq_, midiCursor_, end, tmp, kMaxMidiPerBlock, &dropped);
    if (dropped) midiDropped_.fetch_add(dropped, std::memory_order_relaxed);
    const std::int64_t lag = start - midiCursor_;  // > 0 only if an earlier block could not dispatch
    for (std::size_t i = 0; i < got; ++i) { MidiEvent e = tmp[i]; e.offset = static_cast<std::uint32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(e.offset) - lag)); add(e); }
    midiCursor_ = end;
  }
  midiWasPlaying_ = playing;
  midiLastEnd_ = end;
  // key preview: starts at the top of the block
  for (;;) {
    const auto t = audTail_.load(std::memory_order_relaxed);
    if (t == audHead_.load(std::memory_order_acquire)) break;
    MidiEvent e = aud_[t];
    audTail_.store((t + 1) % aud_.size(), std::memory_order_release);
    e.offset = 0;
    add(e);
  }
  if (audPanic_.exchange(false, std::memory_order_acq_rel)) { MidiEvent off; off.type = MidiEvent::AllNotesOff; add(off); }
  if (n == 0) return;
  for (auto* t : targets_) t->handleMidi(buf, n);
}

void TrackSource::setParamAutomation(std::shared_ptr<const ParamAutomation> a) {
  if (a) { a->last.assign(a->lanes.size(), std::nanf("")); }
  while (autoLock_.test_and_set(std::memory_order_acquire)) {}
  autoRetired_.push_back(autoPending_);
  if (autoRetired_.size() > 8) autoRetired_.erase(autoRetired_.begin());
  autoPending_ = std::move(a);
  autoDirty_.store(true, std::memory_order_release);
  autoLock_.clear(std::memory_order_release);
}

void TrackSource::applyParamAutomation(std::size_t blockFrames) {
  if (autoDirty_.load(std::memory_order_acquire) && !autoLock_.test_and_set(std::memory_order_acquire)) {
    autoLive_ = autoPending_;   // the previous snapshot is also in autoRetired_, so nothing is freed here
    autoDirty_.store(false, std::memory_order_release);
    autoLock_.clear(std::memory_order_release);
  }
  const bool playing = ctx_ && ctx_->playing;
  if (!autoLive_) return;
  if (!playing) {   // stopped: parameters keep what the user set; the next play delivers every lane again
    if (autoWasPlaying_) { for (float& v : autoLive_->last) v = std::nanf(""); autoWasPlaying_ = false; }
    return;
  }
  autoWasPlaying_ = true;
  const std::int64_t at = ctx_->chunkStart + static_cast<std::int64_t>(blockFrames / 2);
  const auto& lanes = autoLive_->lanes;
  for (std::size_t i = 0; i < lanes.size() && i < autoLive_->last.size(); ++i) {
    if (lanes[i].lane.empty() || !lanes[i].inst || isSplitHost(lanes[i].inst)) continue;   // wrapped effects are served inside their own block
    const float v = std::clamp(lanes[i].lane.valueAt(at, 0.f), 0.f, 1.f);
    float& last = autoLive_->last[i];
    if (last == last && std::fabs(v - last) <= 1e-5f) continue;
    if (lanes[i].inst->automateParameter(lanes[i].id, v)) last = v;   // busy or unsupported: tried again next block
  }
}

std::size_t TrackSource::autoBoundariesFor(const plugins::IPluginInstance* inst, std::size_t frames, std::size_t* out, std::size_t maxOut, std::int64_t& chunkStart) {
  chunkStart = ctx_ ? ctx_->chunkStart : 0;
  if (!autoLive_ || !ctx_ || !ctx_->playing || frames < 2 || maxOut < 3) return 0;
  const std::int64_t a = chunkStart, b = chunkStart + static_cast<std::int64_t>(frames);
  bool moving = false, any = false;
  std::size_t n = 0;
  auto add = [&](std::size_t off) { if (n < maxOut - 1) out[n++] = off; };
  add(0);
  for (const auto& l : autoLive_->lanes) {
    if (l.inst != inst || l.lane.empty()) continue;
    any = true;
    for (const auto& p : l.lane.points()) if (p.frame > a && p.frame < b) add(static_cast<std::size_t>(p.frame - a));
    if (std::fabs(l.lane.valueAt(a, 0.f) - l.lane.valueAt(b - 1, 0.f)) > 1e-6f) moving = true;
  }
  if (!any) return 0;
  if (moving) for (std::size_t off = 32; off < frames; off += 32) add(off);
  std::sort(out, out + n);
  n = static_cast<std::size_t>(std::unique(out, out + n) - out);
  if (n >= maxOut) n = maxOut - 1;
  out[n++] = frames;
  return n;
}

void TrackSource::autoApplyFor(plugins::IPluginInstance* inst, std::int64_t at) {
  if (!autoLive_ || !ctx_ || !ctx_->playing) return;
  const auto& lanes = autoLive_->lanes;
  for (std::size_t i = 0; i < lanes.size() && i < autoLive_->last.size(); ++i) {
    if (lanes[i].inst != inst || lanes[i].lane.empty()) continue;
    const float v = std::clamp(lanes[i].lane.valueAt(at, 0.f), 0.f, 1.f);
    float& last = autoLive_->last[i];
    if (last == last && std::fabs(v - last) <= 1e-5f) continue;
    if (inst->automateParameter(lanes[i].id, v)) last = v;
  }
}

void TrackSource::process(const AudioBuffer&, AudioBuffer& out) {
  out.clearValid();
  if (!ctx_) return;
  dispatchMidi(out.frames);
  applyParamAutomation(out.frames);
  const std::int64_t start = ctx_->chunkStart;
  const bool armed = armed_.load(std::memory_order_relaxed);
  const bool rec = armed && ctx_->recording;
  const MonitorMode mm = monitorMode();

  // ---- recording ----
  if (rec && maxFrames_ && out.frames <= maxFrames_) {
    const float* ptrs[2] = {ctx_->inputChannel(inputs_[0]), ctx_->inputChannel(inputs_.size() > 1 ? inputs_[1] : inputs_[0])};
    if (!recording_) {
      std::int64_t s = start - ctx_->recordLatencyFrames;  // the take belongs where the sound happened
      std::int64_t skip = 0;
      if (s < 0) { skip = -s; s = 0; }
      recording_ = pushSlot(kStart, s, skip, nullptr, 0, false);
    }
    if (recording_ && !pushSlot(kData, 0, 0, ptrs, static_cast<std::uint32_t>(out.frames), true))
      dropped_.fetch_add(1, std::memory_order_relaxed);
  } else if (recording_) {
    pushSlot(kStop, 0, 0, nullptr, 0, false);
    recording_ = false;
  }

  // ---- playback (an armed track does not play its tape while recording) ----
  if (ctx_->playing && !rec) {
    if (!lock_.test_and_set(std::memory_order_acquire)) {
      renderClips(out, start);
      lock_.clear(std::memory_order_release);
    } else {
      skips_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // ---- input monitoring ----
  const bool monitor = armed && mm != MonitorMode::Off && (mm == MonitorMode::Input || !ctx_->playing || ctx_->recording);
  if (monitor) {
    for (std::size_t c = 0; c < out.channels; ++c) {
      const unsigned dev = inputs_.size() > 1 ? inputs_[std::min<std::size_t>(c, 1)] : inputs_[0];
      const float* in = ctx_->inputChannel(dev);
      float* y = out.channel(c);
      for (std::size_t i = 0; i < out.frames; ++i) y[i] += in[i];
    }
  }
}

void TrackSource::writerLoop() {
  io::WavWriter wav;
  bool open = false;
  std::int64_t skip = 0, written = 0, startFrame = 0;
  std::string path;
  unsigned ch = 1;
  auto finish = [&] {
    if (!open) return;
    std::string err;
    if (!wav.finalize(&err)) setError("closing " + path + ": " + err);
    Take t;
    t.path = path; t.startFrame = startFrame; t.frames = written; t.channels = ch;
    { std::lock_guard<std::mutex> l(takesMutex_); takes_.push_back(t); }
    open = false;
    writerOpen_.store(false, std::memory_order_release);
  };
  bool failed = false;
  for (;;) {
    bool did = false;
    for (;;) {
      std::unique_lock<std::mutex> cl(cfgMutex_);
      const std::size_t t = tail_.load(std::memory_order_relaxed);
      if (t == head_.load(std::memory_order_acquire)) break;
      const Slot s = slots_[t];
      did = true;
      if (s.type == kStart) {
        finish();
        failed = false;
        std::error_code ec;
        fs::create_directories(dir_, ec);
        // Never overwrite an existing file: pick the next free take number.
        do {
          char num[16];
          std::snprintf(num, sizeof num, "_%03d", ++takeCounter_);
          path = (fs::path(dir_) / (base_ + num + ".wav")).string();
        } while (fs::exists(path));
        ch = static_cast<unsigned>(recCh_);
        std::string err;
        startFrame = s.pos;
        // The BWF time reference stores where the take belongs on the timeline.
        if (wav.open(path, ch, static_cast<unsigned>(sampleRate_), wavType_, &err, base_, static_cast<std::uint64_t>(s.pos))) {
          open = true; written = 0; skip = s.skip;
          writerOpen_.store(true, std::memory_order_release);
        } else {
          failed = true;
          setError("cannot create " + path + ": " + err);
        }
      } else if (s.type == kData && open && !failed) {
        const float* d = slotData_.data() + t * recCh_ * maxFrames_;
        std::int64_t from = std::min<std::int64_t>(skip, s.frames);
        skip -= from;
        const float* planar[2] = {d + from, d + maxFrames_ + from};
        const std::size_t n = s.frames - static_cast<std::size_t>(from);
        if (n) {
          if (wav.write(planar, n)) written += static_cast<std::int64_t>(n);
          else { failed = true; setError("disk write failed for " + path + " (disk full?)"); }
        }
      } else if (s.type == kStop) {
        finish();
      }
      tail_.store((t + 1) % slotCount_, std::memory_order_release);
    }
    if (quit_.load()) { finish(); return; }
    if (!did) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}  // namespace wezt::session
