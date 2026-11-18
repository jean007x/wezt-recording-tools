#pragma once
// Wezt Synth: the built-in polyphonic instrument. Subtractive synthesis (the same sound engine as
// the offline renderer in music/synth.h, now playing live from MIDI) plus a drum kit mode. It is
// an IPluginInstance, so it goes on a track as an insert and plays whatever MIDI the track's
// sequencer or the key preview sends, sample-accurately. Its parameters are on the universal
// control surface, so a person, a script or an AI agent can change them the same way.
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/audio_buffer.h"
#include "plugins/param_table.h"
#include "plugins/plugin_host.h"

namespace wezt::instruments {

class WeztSynth : public plugins::IPluginInstance {
 public:
  WeztSynth();
  ~WeztSynth() override;

  // Loads a preset: keys, bass, pad, lead, pluck, or drums (a General MIDI drum kit).
  void setPreset(const std::string& name);
  const std::string& preset() const { return preset_; }
  static std::vector<std::string> presetNames();

  void prepare(double sampleRate, std::size_t maxFrames) override;
  void process(const AudioBuffer& in, AudioBuffer& out) override;

  bool acceptsMidi() const override { return true; }
  void handleMidi(const MidiEvent* events, std::size_t count) override;
  int activeVoices() const { return active_.load(std::memory_order_relaxed); }
  unsigned droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

  std::size_t parameterCount() const override { return table_.size(); }
  std::string displayName() const override { return "Wezt Synth"; }
  std::vector<plugins::ParamInfo> parameters() const override { return table_.infos(); }
  double parameterNormalized(unsigned id) const override { return id < table_.size() ? table_.toNormalized(id, table_.plain(id)) : 0.0; }
  bool setParameterNormalized(unsigned id, double n) override { if (id >= table_.size() || !std::isfinite(n)) return false; table_.setNormalized(id, n); return true; }
  bool supportsAutomation() const override { return true; }
  bool automateParameter(unsigned id, double n) override { if (id >= table_.size() || !std::isfinite(n)) return false; table_.setNormalized(id, n); return true; }   // atomics: audio-thread safe
  std::string parameterText(unsigned id, double n) const override { return id < table_.size() ? table_.text(id, n) : std::string(); }
  std::vector<std::uint8_t> saveState() const override;
  bool loadState(const std::vector<std::uint8_t>& s) override;

  static plugins::PluginDescriptor descriptor() { plugins::PluginDescriptor d; d.name = "Wezt Synth"; d.vendor = "Wezt"; d.format = plugins::Format::Builtin; d.bitness = 64; return d; }

 private:
  struct Voice;
  struct Drum;
  struct Snap;
  void apply(const MidiEvent& e);
  void noteOn(int key, int vel);
  void noteOff(int key);
  void renderVoices(float* l, float* r, std::size_t n);

  plugins::ParamTable table_;
  std::string preset_ = "keys";
  double rate_ = 48000.0;
  std::array<MidiEvent, kMaxMidiPerBlock> midi_{};
  std::size_t midiN_ = 0;
  std::unique_ptr<std::array<Voice, 32>> voices_;
  std::vector<Drum> drums_;
  std::map<int, AudioBuffer> kit_;
  std::vector<float> scratchL_, scratchR_;
  std::uint64_t age_ = 0;
  bool sustain_ = false;
  float bend_ = 0.f;  // semitones
  float mod_ = 0.f;   // mod wheel 0..1: vibrato depth
  double vibPhase_ = 0.0;
  std::atomic<int> active_{0};
  std::atomic<unsigned> dropped_{0};
};

}  // namespace wezt::instruments
