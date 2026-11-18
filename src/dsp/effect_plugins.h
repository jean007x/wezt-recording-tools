#pragma once
// Wezt's built-in effects as plugins: EQ, compressor, limiter and stereo width. Each is an
// IPluginInstance with its parameters on the universal control surface, so they can sit on a track as
// inserts, be saved with the session, and be driven by a person, a script or an AI agent exactly like
// a third-party VST3/CLAP effect. The signal processing is the existing code in dsp/effects.h.
//
// Threading: the control thread (UI, MCP, agent) sets parameters; the audio thread reads them from
// atomics. The EQ rebuilds its filter set on the control thread (the filters are swapped in at the next
// block without waiting), so only one control thread may set parameters at a time.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dsp/effects.h"
#include "plugins/param_table.h"
#include "plugins/plugin_host.h"

namespace wezt::dsp {

// Common parts: the parameter table, state, and the control surface.
class EffectPlugin : public plugins::IPluginInstance {
 public:
  EffectPlugin(std::string name, std::vector<plugins::ParamDef> defs) : name_(std::move(name)), table_(std::move(defs)) {}
  std::size_t parameterCount() const override { return table_.size(); }
  std::string displayName() const override { return name_; }
  std::vector<plugins::ParamInfo> parameters() const override { return table_.infos(); }
  double parameterNormalized(unsigned id) const override { return id < table_.size() ? table_.toNormalized(id, table_.plain(id)) : 0.0; }
  bool setParameterNormalized(unsigned id, double n) override {
    if (id >= table_.size() || !std::isfinite(n)) return false;
    table_.setNormalized(id, n);
    changed();
    return true;
  }
  bool supportsAutomation() const override { return true; }
  bool automateParameter(unsigned id, double n) override {   // audio thread: atomics, plus a fixed-size hand-off for the EQ
    if (id >= table_.size() || !std::isfinite(n)) return false;
    table_.setNormalized(id, n);
    return automated();
  }
  std::string parameterText(unsigned id, double n) const override { return id < table_.size() ? table_.text(id, n) : std::string(); }
  std::vector<std::uint8_t> saveState() const override { const std::string t = table_.serialize(); return std::vector<std::uint8_t>(t.begin(), t.end()); }
  bool loadState(const std::vector<std::uint8_t>& s) override { const bool ok = table_.deserialize(std::string(s.begin(), s.end())); changed(); return ok; }

 protected:
  virtual void changed() {}  // control thread: a parameter was set
  virtual bool automated() { return true; }   // audio thread: after an automated change; false = try again next block
  std::string name_;
  plugins::ParamTable table_;
};

// Four bands: low shelf, two bells, high shelf.
class EqPlugin : public EffectPlugin {
 public:
  enum : unsigned { LowFreq, LowGain, Mid1Freq, Mid1Gain, Mid1Q, Mid2Freq, Mid2Gain, Mid2Q, HighFreq, HighGain };
  EqPlugin() : EffectPlugin("Wezt EQ", {
      {"Low freq", "Hz", 30, 500, 100, 0, true, "frequency", "Low shelf"},
      {"Low gain", "dB", -15, 15, 0, 0, false, "bass", "Low shelf"},
      {"Low-mid freq", "Hz", 100, 4000, 400, 0, true, "frequency", "Low-mid bell"},
      {"Low-mid gain", "dB", -15, 15, 0, 0, false, "lowmid", "Low-mid bell"},
      {"Low-mid Q", "", 0.3, 8, 1, 0, true, "q", "Low-mid bell"},
      {"High-mid freq", "Hz", 500, 12000, 2500, 0, true, "frequency", "High-mid bell"},
      {"High-mid gain", "dB", -15, 15, 0, 0, false, "himid", "High-mid bell"},
      {"High-mid Q", "", 0.3, 8, 1, 0, true, "q", "High-mid bell"},
      {"High freq", "Hz", 2000, 16000, 8000, 0, true, "frequency", "High shelf"},
      {"High gain", "dB", -15, 15, 0, 0, false, "treble", "High shelf"}}) { push(); }
  void prepare(double sr, std::size_t n) override { eq_.prepare(sr, n); push(); }
  void process(const AudioBuffer& in, AudioBuffer& out) override { eq_.process(in, out); }
  static plugins::PluginDescriptor descriptor() { plugins::PluginDescriptor d; d.name = "Wezt EQ"; d.vendor = "Wezt"; d.format = plugins::Format::Builtin; d.bitness = 64; return d; }

 protected:
  void changed() override { push(); }
  bool automated() override {   // no allocation: bands go into a preallocated slot
    const auto& t = table_;
    const EqBand b[4] = {{FilterType::LowShelf, t.f(LowFreq), t.f(LowGain), 0.707f, true},
                         {FilterType::Peaking, t.f(Mid1Freq), t.f(Mid1Gain), t.f(Mid1Q), true},
                         {FilterType::Peaking, t.f(Mid2Freq), t.f(Mid2Gain), t.f(Mid2Q), true},
                         {FilterType::HighShelf, t.f(HighFreq), t.f(HighGain), 0.707f, true}};
    return eq_.trySetBands(b, 4);
  }

 private:
  void push() {
    const auto& t = table_;
    eq_.setBands({{FilterType::LowShelf, t.f(LowFreq), t.f(LowGain), 0.707f, true},
                  {FilterType::Peaking, t.f(Mid1Freq), t.f(Mid1Gain), t.f(Mid1Q), true},
                  {FilterType::Peaking, t.f(Mid2Freq), t.f(Mid2Gain), t.f(Mid2Q), true},
                  {FilterType::HighShelf, t.f(HighFreq), t.f(HighGain), 0.707f, true}});
  }
  ParametricEq eq_;
};

class CompressorPlugin : public EffectPlugin {
 public:
  enum : unsigned { Threshold, Ratio, Attack, Release, Knee, Makeup, Mix };
  CompressorPlugin() : EffectPlugin("Wezt Compressor", {
      {"Threshold", "dB", -60, 0, -18, 0, false, "threshold", ""},
      {"Ratio", ":1", 1, 20, 2, 0, true, "ratio", ""},
      {"Attack", "ms", 0.1, 200, 20, 0, true, "comp_attack", ""},
      {"Release", "ms", 5, 2000, 200, 0, true, "comp_release", ""},
      {"Knee", "dB", 0, 24, 6, 0, false, "knee", ""},
      {"Makeup gain", "dB", 0, 24, 0, 0, false, "makeup", ""},
      {"Mix", "%", 0, 100, 100, 0, false, "mix", ""}}) {}
  void prepare(double sr, std::size_t n) override { comp_.prepare(sr, n); }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    CompressorParams p;
    p.thresholdDb = table_.f(Threshold); p.ratio = table_.f(Ratio); p.attackMs = table_.f(Attack); p.releaseMs = table_.f(Release);
    p.kneeDb = table_.f(Knee); p.makeupDb = table_.f(Makeup); p.mix = table_.f(Mix) * 0.01f;
    comp_.set(p);
    comp_.process(in, out);
  }
  float gainReductionDb() const { return comp_.lastGainReductionDb(); }
  static plugins::PluginDescriptor descriptor() { plugins::PluginDescriptor d; d.name = "Wezt Compressor"; d.vendor = "Wezt"; d.format = plugins::Format::Builtin; d.bitness = 64; return d; }

 private:
  Compressor comp_;
};

class LimiterPlugin : public EffectPlugin {
 public:
  enum : unsigned { Ceiling, Release };
  LimiterPlugin() : EffectPlugin("Wezt Limiter", {
      {"Ceiling", "dB", -12, 0, -1, 0, false, "ceiling", ""},
      {"Release", "ms", 10, 500, 80, 0, true, "release", ""}}) {}
  void prepare(double sr, std::size_t n) override { LimiterParams p; p.ceilingDb = table_.f(Ceiling); p.releaseMs = table_.f(Release); lim_.set(p); lim_.prepare(sr, n); }
  void process(const AudioBuffer& in, AudioBuffer& out) override {
    LimiterParams p; p.ceilingDb = table_.f(Ceiling); p.releaseMs = table_.f(Release);
    lim_.set(p);
    lim_.process(in, out);
  }
  std::size_t latencySamples() const override { return lim_.latencySamples(); }
  static plugins::PluginDescriptor descriptor() { plugins::PluginDescriptor d; d.name = "Wezt Limiter"; d.vendor = "Wezt"; d.format = plugins::Format::Builtin; d.bitness = 64; return d; }

 private:
  Limiter lim_;
};

class WidthPlugin : public EffectPlugin {
 public:
  WidthPlugin() : EffectPlugin("Wezt Stereo Width", {{"Width", "%", 0, 200, 100, 0, false, "width", ""}}) {}
  void process(const AudioBuffer& in, AudioBuffer& out) override { w_.setWidth(table_.f(0) * 0.01f); w_.process(in, out); }
  static plugins::PluginDescriptor descriptor() { plugins::PluginDescriptor d; d.name = "Wezt Stereo Width"; d.vendor = "Wezt"; d.format = plugins::Format::Builtin; d.bitness = 64; return d; }

 private:
  StereoWidth w_;
};

}  // namespace wezt::dsp
