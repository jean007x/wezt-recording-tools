#pragma once
// Polyphonic sampler instrument (clean-room; a Logic-Sampler-class feature set):
//  - zones with key and velocity ranges, root key, tuning, gain, pan, start/end, reverse
//  - forward and ping-pong loops with loop crossfade, one-shot and gated playback
//  - round-robin and random alternation between zones, mono / legato mode, glide
//  - amp envelope, multimode filter with its own envelope, key and velocity tracking
//  - two LFOs (tempo sync, fade-in, phase, polarity, poly/mono, key trigger) to pitch, filter and level, scaled by mod wheel or velocity
//  - pitch envelope, coarse/fine tuning, key tracking switch, drive before the filter, hold stage on every envelope
//  - sustain pedal, pitch bend, all-notes-off, voice stealing without clicks
//  - auto-mapping from file names, equal and transient slicing, patch save / load
// Events come from one producer thread (UI or MIDI) through a lock-free queue and are applied at the
// start of the next audio block. The audio thread never allocates.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/audio_buffer.h"
#include "engine/graph.h"
#include "plugins/plugin_host.h"

namespace wezt::instruments {

enum class LoopMode : std::uint8_t { Off, Forward, PingPong };
enum class FilterType : std::uint8_t { Off, LowPass, HighPass, BandPass };
enum class LfoShape : std::uint8_t { Sine, Triangle, Square, SawUp, SawDown };
enum class Alternation : std::uint8_t { RoundRobin, Random };
enum class SamplerMode : std::uint8_t { Classic, OneShot, Slice };   // what the editor presents; Slice and OneShot zones ignore note-off
enum class LfoVia : std::uint8_t { None, ModWheel, Velocity };        // what scales an LFO's depth

struct Envelope {
  float attack = 0.002f, decay = 0.1f, sustain = 1.f, release = 0.05f;  // seconds, sustain level 0..1
  float hold = 0.f;                                                    // seconds held at full level between attack and decay
};

struct LfoParams {
  bool on = true;
  LfoShape shape = LfoShape::Sine;
  float rateHz = 5.f;
  bool sync = false;               // follow the host tempo: one cycle lasts syncBeats beats
  float syncBeats = 1.f;
  float fadeIn = 0.f;              // seconds from silence to full depth after a note starts
  float phase = 0.f;               // start phase, 0..1
  bool bipolar = true;             // false: output runs 0..1 instead of -1..1
  bool poly = true;                // true: one LFO per voice; false: one LFO shared by every voice
  bool keyTrigger = true;          // per-voice LFO restarts on each note; off = free running
  LfoVia via = LfoVia::None;
  float pitchCents = 0.f, filterOctaves = 0.f, amp = 0.f;  // depths (amp 0..1; negative pitch/filter depths invert)
};

struct Zone {
  std::string file;                              // source WAV, for patches
  std::shared_ptr<const AudioBuffer> audio;      // null = missing sample, the zone is silent
  double sampleRate = 48000.0;
  int keyLo = 0, keyHi = 127, velLo = 1, velHi = 127;
  int rootKey = 60;
  float tuneCents = 0.f, gainDb = 0.f, pan = 0.f;  // pan -1..1
  std::int64_t start = 0, end = -1;              // frames used; end < 0 = to the end
  LoopMode loop = LoopMode::Off;
  std::int64_t loopStart = 0, loopEnd = -1;      // loopEnd < 0 = the end
  std::int64_t loopCrossfade = 0;                // frames, forward loops
  bool reverse = false;
  bool oneShot = false;                          // ignore note-off
  int rrGroup = 0, rrIndex = 0;                  // rrGroup 0 = no alternation
};

struct SamplerParams {
  int polyphony = 32;                            // 1..64
  bool mono = false, legato = false;
  float glideSeconds = 0.f;
  Envelope amp;
  FilterType filter = FilterType::Off;
  float cutoffHz = 20000.f, resonance = 0.f;     // resonance 0..1
  float filterEnvOctaves = 0.f, filterKeyTrack = 0.f, filterVelOctaves = 0.f;
  Envelope filterEnv{0.002f, 0.2f, 0.f, 0.2f};
  LfoParams lfo[2];
  // pitch section
  float coarseSemitones = 0.f;                   // -24..24, with masterTuneCents as the fine control
  bool pitchKeyTrack = true;                     // false: every key plays the sample at its own pitch
  Envelope pitchEnv{0.f, 0.3f, 0.f, 0.2f};
  float pitchEnvCents = 0.f, pitchEnvVelocity = 0.f;   // depth (can be negative); 0..1 how much velocity scales it
  float filterDrive = 0.f;                       // 0..1 soft saturation before the filter
  SamplerMode mode = SamplerMode::Classic;
  float velocityToAmp = 1.f;                     // 0 = ignore velocity, 1 = full range
  float pitchBendSemitones = 2.f;
  float masterGainDb = 0.f, masterTuneCents = 0.f;
  float pan = 0.f;                               // -1..1, added to every zone's pan
  Alternation alternation = Alternation::RoundRobin;
};

class Sampler : public plugins::IPluginInstance {
 public:
  Sampler();
  ~Sampler() override;

  // ---- setup (call while no events are being sent; not while audio runs) ----
  int addZone(Zone z);                            // returns its index
  void clearZones();
  const std::vector<Zone>& zones() const { return zones_; }
  bool loadSampleFile(const std::string& wav, Zone& zone, std::string* error);  // fills audio, file, sampleRate; keeps other fields
  // Zone per file, root key from the name ("Piano_C#3.wav", "Bass 45.wav"), key ranges split halfway between neighbours.
  int autoMap(const std::vector<std::string>& files, std::string* error = nullptr);
  // Cuts one sample into `count` equal slices, one per key from `firstKey`.
  int sliceEqual(std::shared_ptr<const AudioBuffer> audio, double sampleRate, int count, int firstKey = 36);
  // Cuts at detected attacks (up to `maxSlices`), one per key from `firstKey`.
  int sliceTransients(std::shared_ptr<const AudioBuffer> audio, double sampleRate, float sensitivity = 0.5f, int maxSlices = 64, int firstKey = 36);
  // Sample positions of detected attacks (the first is always 0). Used by sliceTransients and for snapping in the editor.
  static std::vector<std::int64_t> findTransients(const AudioBuffer& audio, double sampleRate, float sensitivity = 0.5f, int maxSlices = 64);
  void setParams(const SamplerParams& p);
  void setTempo(double bpm) { tempo_.store(static_cast<float>(bpm), std::memory_order_relaxed); }  // for tempo-synced LFOs
  const SamplerParams& params() const { return params_; }

  // ---- patches ----
  std::string savePatch() const;                  // text; samples referenced by path
  bool loadPatch(const std::string& text, std::string* error);  // missing samples load as silent zones and are reported

  // ---- events (single producer thread) ----
  bool noteOn(int key, int velocity);
  bool noteOff(int key);
  bool controller(int cc, int value);             // 1 mod wheel, 64 sustain, 123 all notes off
  bool pitchBend(int value14);                    // 0..16383, 8192 centre
  void allNotesOff() { controller(123, 0); }

  int activeVoices() const { return active_.load(std::memory_order_relaxed); }

  // IPluginInstance: the sampler is a built-in instrument. As an insert it adds its sound to what comes in.
  std::size_t parameterCount() const override;
  std::string displayName() const override { return "Wezt Sampler"; }
  std::vector<plugins::ParamInfo> parameters() const override;
  double parameterNormalized(unsigned id) const override;
  bool setParameterNormalized(unsigned id, double normalized) override;
  bool automateParameter(unsigned id, double normalized) override;
  bool supportsAutomation() const override { return true; }
  std::string parameterText(unsigned id, double normalized) const override;
  // MIDI from the track's sequencer or the key preview (audio thread, just before process()).
  bool acceptsMidi() const override { return true; }
  void handleMidi(const MidiEvent* events, std::size_t count) override;
  std::vector<std::uint8_t> saveState() const override { const std::string t = savePatch(); return std::vector<std::uint8_t>(t.begin(), t.end()); }
  bool loadState(const std::vector<std::uint8_t>& s) override { return loadPatch(std::string(s.begin(), s.end()), nullptr); }
  static plugins::PluginDescriptor descriptor() { plugins::PluginDescriptor d; d.name = "Wezt Sampler"; d.vendor = "Wezt"; d.format = plugins::Format::Builtin; d.bitness = 64; return d; }

  // ---- editing while audio runs ----
  // Everything that changes zones or parameters takes a short spin lock; if the audio thread meets
  // it, that block is silent (an edit takes microseconds). Editing a zone or removing one stops the
  // notes that were using zones.
  void updateZone(std::size_t index, Zone z);
  bool removeZone(std::size_t index);

  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;

  static int noteNameToKey(const std::string& text);  // "C3" = 60, "A#-1", "60"; -1 if none found

 private:
  struct Event { std::uint8_t type = 0; std::int16_t a = 0, b = 0; };
  struct Voice;
  bool push(Event e);
  void handle(const Event& e);
  void startNote(int key, int vel);
  void releaseKey(int key);
  Voice* allocate();
  void render(Voice& v, float* l, float* r, std::size_t n);

  std::vector<Zone> zones_;
  SamplerParams params_;
  double rate_ = 48000.0;

  static constexpr std::size_t kQueue = 1024;
  std::array<Event, kQueue> queue_{};
  std::array<MidiEvent, kMaxMidiPerBlock> midi_{};   // this block's sequencer events (audio thread only)
  std::size_t midiN_ = 0;
  std::atomic<std::size_t> head_{0}, tail_{0};

  std::unique_ptr<std::array<Voice, 64>> voices_;
  std::array<int, 64> rrCounter_{};
  std::uint32_t rng_ = 22222u;
  std::uint64_t age_ = 0;
  bool sustain_ = false;
  float modWheel_ = 0.f, bend_ = 0.f;
  int lastKey_ = -1;
  float glideFrom_ = 0.f;
  std::atomic<float> tempo_{120.f};
  double gPhase_[2] = {0.0, 0.0}, gStart_[2] = {0.0, 0.0};   // shared LFO phases (block start)
  std::atomic<int> active_{0};
  std::vector<float> scratchL_, scratchR_;
  mutable std::atomic_flag edit_ = ATOMIC_FLAG_INIT;
  struct EditGuard;
  void killVoices();
};

}  // namespace wezt::instruments
