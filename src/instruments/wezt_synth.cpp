#include "instruments/wezt_synth.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/denormals.h"
#include "music/midi.h"
#include "music/synth.h"

namespace wezt::instruments {
namespace {
constexpr double kPi = 3.14159265358979323846;

enum P { Saw, Square, Tri, Sine, Sub, Unison, Detune, Cutoff, EnvCutoff, Attack, Decay, Sustain, Release, Gain, Pan, Mode, Count };

std::vector<plugins::ParamDef> defs() {
  return {
      {"Saw", "", 0, 1, 0.5, 0, false, "level", "Oscillator"},
      {"Square", "", 0, 1, 0, 0, false, "level", "Oscillator"},
      {"Triangle", "", 0, 1, 0, 0, false, "level", "Oscillator"},
      {"Sine", "", 0, 1, 0, 0, false, "level", "Oscillator"},
      {"Sub", "", 0, 1, 0, 0, false, "level", "Oscillator"},
      {"Unison voices", "", 1, 4, 1, 3, false, "unison", "Oscillator"},
      {"Detune", "cents", 0, 50, 8, 0, false, "detune", "Oscillator"},
      {"Cutoff", "Hz", 40, 18000, 3000, 0, true, "cutoff", "Filter"},
      {"Envelope to cutoff", "Hz", 0, 8000, 0, 0, false, "envelope", "Filter"},
      {"Attack", "s", 0.001, 5, 0.005, 0, true, "attack", "Envelope"},
      {"Decay", "s", 0.01, 5, 0.2, 0, true, "decay", "Envelope"},
      {"Sustain", "", 0, 1, 0.7, 0, false, "sustain", "Envelope"},
      {"Release", "s", 0.005, 10, 0.2, 0, true, "release", "Envelope"},
      {"Gain", "dB", -36, 6, -6, 0, false, "gain", "Output"},
      {"Pan", "", -1, 1, 0, 0, false, "pan", "Output"},
      {"Drum kit mode", "", 0, 1, 0, 1, false, "mode", "Output"},
  };
}

inline double polyBlep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
  if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
  return 0.0;
}
inline float panL(float p) { return std::cos((p + 1.f) * static_cast<float>(kPi) / 4.f); }
inline float panR(float p) { return std::sin((p + 1.f) * static_cast<float>(kPi) / 4.f); }
}  // namespace

struct WeztSynth::Snap {
  float saw, square, tri, sine, sub, detune, cutoff, envCut, attack, decay, sustain, release, gain, pan;
  int unison;
  bool drums;
};

struct WeztSynth::Voice {
  bool active = false, held = false, sustained = false, releasing = false;
  int key = 0;
  float vel = 0.f;
  std::uint64_t age = 0;
  double phase[4] = {0, 0, 0, 0}, subPhase = 0;
  float lpA = 0, lpB = 0, lpC = 0, lpD = 0;
  std::size_t t = 0;       // samples since the note started
  float env = 0, relStart = 0;
  std::size_t relPos = 0;
};

struct WeztSynth::Drum {
  const AudioBuffer* buf = nullptr;
  std::size_t pos = 0;
  float gain = 0.f;
};

WeztSynth::WeztSynth() : table_(defs()), voices_(std::make_unique<std::array<Voice, 32>>()) { setPreset("keys"); }
WeztSynth::~WeztSynth() = default;

std::vector<std::string> WeztSynth::presetNames() { return {"keys", "bass", "pad", "lead", "pluck", "drums"}; }

void WeztSynth::setPreset(const std::string& name) {
  const bool drums = name == "drums";
  const music::SynthPreset p = music::synthPreset(drums ? "keys" : name);
  preset_ = drums ? "drums" : p.name;
  auto set = [&](int i, double v) { table_.setPlain(static_cast<std::size_t>(i), v); };
  set(Saw, p.saw); set(Square, p.square); set(Tri, p.tri); set(Sine, p.sine); set(Sub, p.subLevel);
  set(Unison, p.unison); set(Detune, p.detuneCents); set(Cutoff, p.cutoffHz); set(EnvCutoff, p.envToCutoff);
  set(Attack, p.attack); set(Decay, p.decay); set(Sustain, p.sustain); set(Release, p.release); set(Gain, p.gainDb); set(Pan, p.pan);
  set(Mode, drums ? 1 : 0);
}

void WeztSynth::prepare(double sampleRate, std::size_t maxFrames) {
  rate_ = sampleRate;
  scratchL_.assign(std::max<std::size_t>(maxFrames, 64), 0.f);
  scratchR_.assign(std::max<std::size_t>(maxFrames, 64), 0.f);
  for (auto& v : *voices_) v = Voice();
  drums_.clear();
  drums_.reserve(64);  // note-on never allocates on the audio thread
  midiN_ = 0; sustain_ = false; bend_ = 0.f; mod_ = 0.f; vibPhase_ = 0.0;
  // The drum kit is synthesised once per sample rate, one hit per General MIDI drum note, and played back as one-shots.
  kit_.clear();
  for (int pitch : {35, 36, 37, 38, 39, 41, 42, 43, 45, 46, 47, 48, 49, 51}) {
    music::MidiClip c;
    c.notes.push_back({pitch, 0.0, 0.25, 127});
    kit_.emplace(pitch, music::renderDrums(c, 120.0, sampleRate));
  }
}

void WeztSynth::handleMidi(const MidiEvent* events, std::size_t count) {
  std::size_t kept = 0;  // releases left over from a block that never ran
  for (std::size_t i = 0; i < midiN_; ++i)
    if (midi_[i].type == MidiEvent::NoteOff || midi_[i].type == MidiEvent::AllNotesOff) { midi_[kept] = midi_[i]; midi_[kept++].offset = 0; }
  midiN_ = kept;
  for (std::size_t i = 0; i < count; ++i) {
    if (midiN_ >= midi_.size()) { dropped_.fetch_add(1, std::memory_order_relaxed); continue; }
    midi_[midiN_++] = events[i];
  }
  std::stable_sort(midi_.begin(), midi_.begin() + static_cast<std::ptrdiff_t>(midiN_), [](const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
}

void WeztSynth::noteOn(int key, int vel) {
  if (key < 0 || key > 127) return;
  const float v = std::pow(static_cast<float>(std::clamp(vel, 1, 127)) / 127.f, 1.5f);
  if (table_.plain(Mode) >= 0.5) {
    auto it = kit_.find(key);
    if (it == kit_.end()) return;
    const float g = std::pow(static_cast<float>(std::clamp(vel, 1, 127)) / 127.f, 1.3f);
    if (drums_.size() < 64) drums_.push_back({&it->second, 0, g});  // capacity grows only here, in the first blocks of a session
    return;
  }
  Voice* pick = nullptr;
  for (auto& vo : *voices_) if (!vo.active) { pick = &vo; break; }
  if (!pick) {
    for (auto& vo : *voices_) if (vo.releasing && (!pick || vo.env < pick->env)) pick = &vo;
    if (!pick) for (auto& vo : *voices_) if (!pick || vo.age < pick->age) pick = &vo;
  }
  *pick = Voice();
  pick->active = true; pick->held = true; pick->key = key; pick->vel = v; pick->age = ++age_;
  for (int u = 0; u < 4; ++u) pick->phase[u] = std::fmod(0.37 * u + key * 0.113, 1.0);
}

void WeztSynth::noteOff(int key) {
  for (auto& v : *voices_) {
    if (!v.active || !v.held || v.key != key) continue;
    v.held = false;
    if (sustain_) v.sustained = true;
    else { v.releasing = true; v.relStart = v.env; v.relPos = 0; }
  }
}

void WeztSynth::apply(const MidiEvent& e) {
  switch (e.type) {
    case MidiEvent::NoteOn: if (e.b > 0) noteOn(e.a, e.b); else noteOff(e.a); break;
    case MidiEvent::NoteOff: noteOff(e.a); break;
    case MidiEvent::PitchBend: bend_ = (static_cast<float>(std::clamp<int>(e.a, 0, 16383)) - 8192.f) / 8192.f * 2.f; break;
    case MidiEvent::Controller:
      if (e.a == 1) mod_ = static_cast<float>(std::clamp<int>(e.b, 0, 127)) / 127.f;
      else if (e.a == 64) {
        const bool down = e.b >= 64;
        if (sustain_ && !down) for (auto& v : *voices_) if (v.active && v.sustained) { v.sustained = false; v.releasing = true; v.relStart = v.env; v.relPos = 0; }
        sustain_ = down;
      } else if (e.a == 123) {
        sustain_ = false;
        for (auto& v : *voices_) if (v.active && !v.releasing) { v.held = false; v.sustained = false; v.releasing = true; v.relStart = v.env; v.relPos = 0; }
      }
      break;
    case MidiEvent::AllNotesOff:
      sustain_ = false;
      for (auto& v : *voices_) if (v.active && !v.releasing) { v.held = false; v.sustained = false; v.releasing = true; v.relStart = v.env; v.relPos = 0; }
      break;
  }
}

void WeztSynth::renderVoices(float* L, float* R, std::size_t n) {
  Snap s;
  s.saw = table_.f(Saw); s.square = table_.f(Square); s.tri = table_.f(Tri); s.sine = table_.f(Sine); s.sub = table_.f(Sub);
  s.detune = table_.f(Detune); s.cutoff = table_.f(Cutoff); s.envCut = table_.f(EnvCutoff);
  s.attack = std::max(1e-4f, table_.f(Attack)); s.decay = std::max(1e-3f, table_.f(Decay)); s.sustain = table_.f(Sustain); s.release = std::max(0.005f, table_.f(Release));
  s.gain = std::pow(10.f, table_.f(Gain) / 20.f); s.pan = table_.f(Pan); s.unison = std::clamp(static_cast<int>(std::lround(table_.plain(Unison))), 1, 4);
  s.drums = table_.plain(Mode) >= 0.5;
  const float lp = panL(s.pan), rp = panR(s.pan);
  const std::size_t relLen = static_cast<std::size_t>(s.release * static_cast<float>(rate_));
  // mod wheel: 5.5 Hz vibrato up to +-0.5 semitone, taken once per block (the same value for every voice)
  vibPhase_ += 5.5 * static_cast<double>(n) / rate_; vibPhase_ -= std::floor(vibPhase_);
  const float vib = mod_ * 0.5f * static_cast<float>(std::sin(2.0 * kPi * vibPhase_));
  const float bendMul = std::pow(2.f, (bend_ + vib) / 12.f);
  for (auto& v : *voices_) {
    if (!v.active) continue;
    const double f0 = 440.0 * std::pow(2.0, (v.key - 69) / 12.0) * bendMul;
    const int U = s.unison;
    double inc[4]; float vl[4], vr[4];
    for (int u = 0; u < U; ++u) {
      const double spread = U == 1 ? 0.0 : (static_cast<double>(u) / (U - 1) * 2.0 - 1.0);
      inc[u] = f0 * std::pow(2.0, spread * s.detune / 1200.0) / rate_;
      vl[u] = panL(static_cast<float>(spread) * 0.7f) / std::sqrt(static_cast<float>(U));
      vr[u] = panR(static_cast<float>(spread) * 0.7f) / std::sqrt(static_cast<float>(U));
    }
    for (std::size_t i = 0; i < n; ++i) {
      const float tsec = static_cast<float>(v.t) / static_cast<float>(rate_);
      if (v.releasing) {
        const float r = relLen ? static_cast<float>(v.relPos) / static_cast<float>(relLen) : 1.f;
        v.env = v.relStart * std::max(0.f, 1.f - r);
        ++v.relPos;
        if (v.relPos >= relLen) { v.active = false; break; }
      } else if (tsec < s.attack) v.env = tsec / s.attack;
      else v.env = s.sustain + (1.f - s.sustain) * std::exp(-4.f * (tsec - s.attack) / s.decay);
      ++v.t;
      float l = 0.f, r = 0.f;
      for (int u = 0; u < U; ++u) {
        double& ph = v.phase[u];
        const double dt = inc[u];
        float x = 0.f;
        if (s.sine > 0) x += s.sine * static_cast<float>(std::sin(2.0 * kPi * ph));
        if (s.tri > 0) x += s.tri * static_cast<float>(4.0 * std::fabs(ph - 0.5) - 1.0);
        if (s.saw > 0) x += s.saw * static_cast<float>(2.0 * ph - 1.0 - polyBlep(ph, dt));
        if (s.square > 0) { double q = ph < 0.5 ? 1.0 : -1.0; q += polyBlep(ph, dt); q -= polyBlep(std::fmod(ph + 0.5, 1.0), dt); x += s.square * static_cast<float>(q); }
        ph += dt; if (ph >= 1.0) ph -= 1.0;
        l += x * vl[u]; r += x * vr[u];
      }
      if (s.sub > 0) {
        const float sub = s.sub * static_cast<float>(std::sin(2.0 * kPi * v.subPhase));
        v.subPhase += f0 * 0.5 / rate_; if (v.subPhase >= 1.0) v.subPhase -= 1.0;
        l += sub; r += sub;
      }
      const float cut = std::clamp(s.cutoff + s.envCut * v.env * (0.4f + 0.6f * v.vel), 40.f, static_cast<float>(rate_) * 0.45f);
      const float a = 1.f - std::exp(-2.f * static_cast<float>(kPi) * cut / static_cast<float>(rate_));
      v.lpA += a * (l - v.lpA); v.lpB += a * (v.lpA - v.lpB);
      v.lpC += a * (r - v.lpC); v.lpD += a * (v.lpC - v.lpD);
      const float g = v.env * v.vel * s.gain * 1.41f;
      L[i] += v.lpB * g * lp;
      R[i] += v.lpD * g * rp;
    }
  }
}

void WeztSynth::process(const AudioBuffer& in, AudioBuffer& out) {
  ScopedNoDenormals nd;
  out.clearValid();
  for (std::size_t c = 0; c < std::min(in.channels, out.channels); ++c) std::copy(in.channel(c), in.channel(c) + std::min(in.frames, out.frames), out.channel(c));
  if (out.channels == 0) return;
  std::size_t off = 0, mi = 0;
  const std::size_t cap = scratchL_.size();
  while (off < out.frames && cap > 0) {
    while (mi < midiN_ && midi_[mi].offset <= off) apply(midi_[mi++]);
    std::size_t n = std::min(cap, out.frames - off);
    if (mi < midiN_) n = std::min(n, std::max<std::size_t>(1, midi_[mi].offset - off));
    std::fill(scratchL_.begin(), scratchL_.begin() + static_cast<std::ptrdiff_t>(n), 0.f);
    std::fill(scratchR_.begin(), scratchR_.begin() + static_cast<std::ptrdiff_t>(n), 0.f);
    renderVoices(scratchL_.data(), scratchR_.data(), n);
    const float dg = std::pow(10.f, table_.f(Gain) / 20.f) * 1.5f;
    for (auto& d : drums_) {
      if (!d.buf) continue;
      const std::size_t len = d.buf->frames, chs = d.buf->channels;
      for (std::size_t i = 0; i < n && d.pos < len; ++i, ++d.pos) {
        scratchL_[i] += d.buf->channel(0)[d.pos] * d.gain * dg;
        scratchR_[i] += d.buf->channel(chs > 1 ? 1 : 0)[d.pos] * d.gain * dg;
      }
    }
    float* l = out.channel(0) + off;
    float* r = out.channels > 1 ? out.channel(1) + off : nullptr;
    for (std::size_t i = 0; i < n; ++i) {
      if (r) { l[i] += scratchL_[i]; r[i] += scratchR_[i]; }
      else l[i] += 0.5f * (scratchL_[i] + scratchR_[i]);
    }
    off += n;
  }
  while (mi < midiN_) { const MidiEvent& m = midi_[mi++]; if (m.type == MidiEvent::NoteOff || m.type == MidiEvent::AllNotesOff) apply(m); }
  midiN_ = 0;
  drums_.erase(std::remove_if(drums_.begin(), drums_.end(), [](const Drum& d) { return !d.buf || d.pos >= d.buf->frames; }), drums_.end());
  int act = 0;
  for (auto& v : *voices_) if (v.active) ++act;
  active_.store(act + static_cast<int>(drums_.size()), std::memory_order_relaxed);
}

std::vector<std::uint8_t> WeztSynth::saveState() const {
  const std::string t = "preset=" + preset_ + "\n" + table_.serialize();
  return std::vector<std::uint8_t>(t.begin(), t.end());
}

bool WeztSynth::loadState(const std::vector<std::uint8_t>& s) {
  const std::string t(s.begin(), s.end());
  if (t.rfind("preset=", 0) != 0) return false;
  const auto nl = t.find('\n');
  if (nl == std::string::npos) return false;
  const std::string name = t.substr(7, nl - 7);
  for (const auto& n : presetNames()) if (n == name) preset_ = name;
  return table_.deserialize(t.substr(nl + 1));
}

}  // namespace wezt::instruments
