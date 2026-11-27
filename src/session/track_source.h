#pragma once
#include <algorithm>
// A track's audio source: plays its clips, monitors the live input, and records armed takes to disk.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/context.h"
#include "engine/graph.h"
#include "io/wav.h"
#include "plugins/plugin_host.h"
#include "session/midi_player.h"
#include "session/strip.h"

namespace wezt::session {

// A region of audio on the timeline. The audio is held in memory (see docs/ARCHITECTURE.md).
struct Clip {
  int id = 0;
  std::string file;                              // source WAV (for saving)
  std::shared_ptr<const AudioBuffer> audio;      // null = file missing; plays as silence
  std::int64_t start = 0;                        // timeline position, in frames
  std::int64_t offset = 0;                       // first frame used from `audio`
  std::int64_t length = 0;                       // frames on the timeline
  float gainDb = 0.f;
  std::int64_t fadeIn = 0, fadeOut = 0;          // linear fades, in frames
  AutomationLane gainEnv;                        // clip gain line: dB over clip-relative frames; empty = none. Applied before inserts and fader.
  std::string name;
  bool muted = false, locked = false;
  std::uint8_t color = 0;                        // 0: the track's colour; 1..8: a colour from the palette, to tell clips apart
};

// A finished recording, as the writer thread left it.
struct Take {
  std::string path;
  std::int64_t startFrame = 0;  // timeline position of the first frame (already latency-compensated)
  std::int64_t frames = 0;
  unsigned channels = 0;
};

enum class MonitorMode {
  Auto,   // hear the input while stopped or recording, the tape while playing
  Input,  // always the input (when armed)
  Off     // never the input
};

class TrackSource : public Processor {
 public:
  TrackSource();
  ~TrackSource() override;

  void setContext(const RtContext* c) { ctx_ = c; }

  // --- playlist (UI thread; safe while playing: the audio thread skips one block at most) ---
  void setPlaylist(std::vector<Clip> clips);
  std::vector<Clip> playlist() const;

  // --- input and recording (configure while stopped) ---
  void setInputs(std::vector<unsigned> deviceChannels);  // 1 or 2 channels
  const std::vector<unsigned>& inputs() const { return inputs_; }
  void setRecordFolder(const std::string& dir, const std::string& baseName, io::WavSampleType type = io::WavSampleType::Int24);
  void arm(bool on);
  bool armed() const { return armed_.load(std::memory_order_relaxed); }
  void setMonitorMode(MonitorMode m) { monitor_.store(static_cast<int>(m), std::memory_order_relaxed); }
  MonitorMode monitorMode() const { return static_cast<MonitorMode>(monitor_.load(std::memory_order_relaxed)); }

  // Waits until the writer thread has written and closed everything handed to it (call after the
  // transport stopped). Returns false on timeout.
  bool waitIdle(int timeoutMs = 5000);
  // Finished takes since the last call.
  std::vector<Take> collectTakes();
  // Problems seen while recording: blocks lost because the disk could not keep up, and write errors.
  std::uint64_t droppedBlocks() const { return dropped_.load(std::memory_order_relaxed); }
  std::uint64_t playbackSkips() const { return skips_.load(std::memory_order_relaxed); }
  std::string lastError() const;

  // --- live MIDI (control thread; safe while playing) ---
  // The track plays `seq` to its instrument targets in step with the transport, sample-accurately.
  // Jumping the playhead or stopping releases every sounding note.
  void setMidiSequence(std::shared_ptr<const MidiSequence> seq);
  std::shared_ptr<const MidiSequence> midiSequence() const;
  // The instruments (inserts that accept MIDI) the sequence and the key preview play. The session
  // keeps this current when inserts change; the pointers stay owned by the graph.
  void setMidiTargets(std::vector<plugins::IPluginInstance*> targets);
  // Click-a-key preview: starts or stops a note on the track's instruments right away (next audio
  // block), whether or not the transport runs. Single producer. False when the queue is full.
  bool auditionNote(int key, int velocity, bool on);
  bool auditionController(int cc, int value);   // sustain pedal (64), mod wheel (1), ... from a hardware keyboard
  bool auditionPitchBend(int value14);          // 0..16383, 8192 = centre
  void auditionAllOff();
  unsigned midiDropped() const { return midiDropped_.load(std::memory_order_relaxed); }

  // --- plugin parameter automation (control thread; safe while playing) ---
  // While the transport plays, every lane sets its plugin parameter once per block (values 0..1). The
  // snapshot is immutable; the audio thread only ever try-locks to pick up a new one.
  struct ParamAutoLane { plugins::IPluginInstance* inst = nullptr; unsigned id = 0; AutomationLane lane; };
  struct ParamAutomation {
    std::vector<ParamAutoLane> lanes;
    mutable std::vector<float> last;   // audio thread: the value last delivered per lane (NaN = none yet)
  };
  void setParamAutomation(std::shared_ptr<const ParamAutomation> a);
  // Effects wrapped by AutomatedEffect take their automation inside the block (see automated_effect.h).
  // addSplitHost/removeSplitHost: control thread, audio stopped. The other two: audio thread, after this source's process() in the same block.
  void addSplitHost(plugins::IPluginInstance* inst) { splitHosts_.push_back(inst); }
  void removeSplitHost(plugins::IPluginInstance* inst) { splitHosts_.erase(std::remove(splitHosts_.begin(), splitHosts_.end(), inst), splitHosts_.end()); }
  // Fills `out` with sorted offsets 0 .. frames (both included) where the plug-in's lanes need a new value; returns how many.
  // 0 when nothing is automated or the transport is stopped. `chunkStart` is the timeline frame of the block's first sample.
  // The MIDI of the block just dispatched (sorted by offset). Instruments wrapped by AutomatedEffect read it here, so they can be run in pieces
  // with each event inside its own piece; the other instruments are handed the events directly. Audio thread, after this source's process().
  const MidiEvent* blockMidi(std::size_t& n) const { n = blockMidiN_; return blockMidi_; }
  std::size_t autoBoundariesFor(const plugins::IPluginInstance* inst, std::size_t frames, std::size_t* out, std::size_t maxOut, std::int64_t& chunkStart);
  void autoApplyFor(plugins::IPluginInstance* inst, std::int64_t atFrame);

  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;

 private:
  // One value per block, taken from the middle of the block: a moving lane is then on average neither ahead nor behind.
  void applyParamAutomation(std::size_t blockFrames);
  std::atomic_flag autoLock_ = ATOMIC_FLAG_INIT;
  std::shared_ptr<const ParamAutomation> autoPending_, autoLive_;
  std::vector<std::shared_ptr<const ParamAutomation>> autoRetired_;   // keeps old snapshots alive so the audio thread never frees one
  std::atomic<bool> autoDirty_{false};
  std::vector<plugins::IPluginInstance*> splitHosts_;
  MidiEvent blockMidi_[kMaxMidiPerBlock];
  std::size_t blockMidiN_ = 0;
  bool isSplitHost(const plugins::IPluginInstance* i) const { return std::find(splitHosts_.begin(), splitHosts_.end(), i) != splitHosts_.end(); }
  bool autoWasPlaying_ = false;
  void dispatchMidi(std::size_t frames);
  bool auditionPush(const MidiEvent& e);

  mutable std::atomic_flag midiLock_ = ATOMIC_FLAG_INIT;   // control thread holds it for microseconds; the audio thread only try-locks
  std::shared_ptr<const MidiSequence> seq_;
  std::vector<plugins::IPluginInstance*> targets_;
  std::array<MidiEvent, 256> aud_{};
  std::atomic<std::size_t> audHead_{0}, audTail_{0};
  std::atomic<bool> audPanic_{false};                      // "all notes off" cannot be lost to a full queue
  std::int64_t midiCursor_ = 0, midiLastEnd_ = -1;         // audio thread
  bool midiWasPlaying_ = false;
  std::atomic<unsigned> midiDropped_{0};

  struct Slot { int type = 0; std::uint32_t frames = 0; std::int64_t pos = 0, skip = 0; };
  enum { kStart = 1, kData = 2, kStop = 3 };

  void renderClips(AudioBuffer& out, std::int64_t chunkStart);
  bool pushSlot(int type, std::int64_t pos, std::int64_t skip, const float* const* planar, std::uint32_t frames, bool reserve);
  void writerLoop();
  void startWriterIfNeeded();
  void setError(const std::string& e);

  const RtContext* ctx_ = nullptr;

  // playlist, guarded by a try-lock the audio thread never waits on
  std::vector<Clip> clips_;
  mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  std::atomic<std::uint64_t> skips_{0};

  std::vector<unsigned> inputs_{0};
  std::atomic<bool> armed_{false};
  std::atomic<int> monitor_{static_cast<int>(MonitorMode::Auto)};

  // recording ring: fixed slots, single producer (audio) / single consumer (writer)
  std::mutex cfgMutex_;
  std::vector<Slot> slots_;
  std::vector<float> slotData_;
  std::size_t slotCount_ = 0, maxFrames_ = 0, recCh_ = 1;
  std::atomic<std::size_t> head_{0}, tail_{0};
  bool recording_ = false;  // audio thread
  std::int64_t recNext_ = -1;  // where the next recorded chunk should start if the song carries on; a jump (loop wrap, locate) ends the take
  double sampleRate_ = 48000;
  std::atomic<std::uint64_t> dropped_{0};

  std::string dir_, base_;
  io::WavSampleType wavType_ = io::WavSampleType::Int24;
  std::thread writer_;
  std::atomic<bool> quit_{false}, writerOpen_{false};
  mutable std::mutex takesMutex_;
  std::vector<Take> takes_;
  std::string error_;
  int takeCounter_ = 0;
};

}  // namespace wezt::session
