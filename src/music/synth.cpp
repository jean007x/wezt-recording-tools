#include "music/synth.h"

#include <algorithm>
#include <cmath>

namespace wezt::music {
namespace {
constexpr double kPi = 3.14159265358979323846;

double polyBlep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
  if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
  return 0.0;
}

struct Noise {
  std::uint32_t s = 22222u;
  float next() { s = s * 1664525u + 1013904223u; return static_cast<float>(static_cast<std::int32_t>(s)) / 2147483648.f; }
};

std::size_t framesFor(double beats, double bpm, double sr) { return static_cast<std::size_t>(std::max(0.0, beats * 60.0 / bpm * sr)); }

float panL(float p) { return std::cos((p * 0.5f + 0.5f) * static_cast<float>(kPi) * 0.5f); }
float panR(float p) { return std::sin((p * 0.5f + 0.5f) * static_cast<float>(kPi) * 0.5f); }

}  // namespace

std::vector<std::string> synthPresetNames() { return {"bass", "keys", "pad", "lead", "pluck"}; }

SynthPreset synthPreset(const std::string& n) {
  SynthPreset p;
  p.name = n;
  if (n == "bass") { p.saw = 0.7f; p.square = 0.3f; p.subLevel = 0.8f; p.cutoffHz = 500; p.envToCutoff = 1500; p.attack = 0.004f; p.decay = 0.18f; p.sustain = 0.6f; p.release = 0.08f; p.gainDb = -4; }
  else if (n == "pad") { p.saw = 0.8f; p.tri = 0.3f; p.unison = 3; p.detuneCents = 14; p.cutoffHz = 1400; p.envToCutoff = 900; p.attack = 0.7f; p.decay = 0.8f; p.sustain = 0.85f; p.release = 1.2f; p.gainDb = -12; }
  else if (n == "lead") { p.saw = 0.5f; p.square = 0.5f; p.unison = 2; p.detuneCents = 7; p.cutoffHz = 3500; p.envToCutoff = 2500; p.attack = 0.01f; p.decay = 0.2f; p.sustain = 0.75f; p.release = 0.25f; p.gainDb = -8; }
  else if (n == "pluck") { p.saw = 0.6f; p.tri = 0.4f; p.cutoffHz = 900; p.envToCutoff = 4000; p.attack = 0.002f; p.decay = 0.22f; p.sustain = 0.0f; p.release = 0.15f; p.gainDb = -8; }
  else { p.name = "keys"; p.tri = 0.6f; p.sine = 0.4f; p.saw = 0.25f; p.unison = 2; p.detuneCents = 5; p.cutoffHz = 2600; p.envToCutoff = 2200; p.attack = 0.006f; p.decay = 0.6f; p.sustain = 0.25f; p.release = 0.3f; p.gainDb = -8; }
  return p;
}

AudioBuffer renderSynth(const MidiClip& clip, const SynthPreset& pr, double bpm, double sr) {
  double endBeat = 0;
  for (auto& n : clip.notes) endBeat = std::max(endBeat, n.start + n.length);
  const std::size_t total = framesFor(endBeat, bpm, sr) + static_cast<std::size_t>((pr.release + 0.1) * sr) + 1;
  AudioBuffer out(2, total);
  const float master = std::pow(10.f, pr.gainDb / 20.f);
  const float lp = panL(pr.pan), rp = panR(pr.pan);
  for (const Note& note : clip.notes) {
    if (note.length <= 0) continue;
    const std::size_t s0 = framesFor(note.start, bpm, sr);
    const std::size_t held = std::max<std::size_t>(1, framesFor(note.length, bpm, sr));
    const std::size_t rel = static_cast<std::size_t>(std::max(0.005f, pr.release) * sr);
    const std::size_t len = held + rel;
    const double f0 = 440.0 * std::pow(2.0, (note.pitch - 69) / 12.0);
    const float vel = std::pow(static_cast<float>(note.velocity) / 127.f, 1.5f);
    const int U = std::max(1, pr.unison);
    std::vector<double> phase(static_cast<std::size_t>(U), 0.0), inc(static_cast<std::size_t>(U));
    std::vector<float> vl(static_cast<std::size_t>(U)), vr(static_cast<std::size_t>(U));
    for (int u = 0; u < U; ++u) {
      const double spread = U == 1 ? 0.0 : (static_cast<double>(u) / (U - 1) * 2.0 - 1.0);
      inc[static_cast<std::size_t>(u)] = f0 * std::pow(2.0, spread * pr.detuneCents / 1200.0) / sr;
      phase[static_cast<std::size_t>(u)] = std::fmod(0.37 * u + note.pitch * 0.113, 1.0);
      vl[static_cast<std::size_t>(u)] = panL(static_cast<float>(spread) * 0.7f) / std::sqrt(static_cast<float>(U));
      vr[static_cast<std::size_t>(u)] = panR(static_cast<float>(spread) * 0.7f) / std::sqrt(static_cast<float>(U));
    }
    double subPhase = 0.0;
    float lpA = 0.f, lpB = 0.f, lpC = 0.f, lpD = 0.f;
    float env = 0.f, relStart = 0.f;
    for (std::size_t i = 0; i < len && s0 + i < total; ++i) {
      // envelope: attack, decay to sustain while held, then release from wherever it was
      const float tsec = static_cast<float>(i) / static_cast<float>(sr);
      if (i < held) {
        if (tsec < pr.attack) env = tsec / std::max(1e-4f, pr.attack);
        else { const float d = (tsec - pr.attack) / std::max(1e-3f, pr.decay); env = pr.sustain + (1.f - pr.sustain) * std::exp(-4.f * d); }
        relStart = env;
      } else {
        const float r = static_cast<float>(i - held) / static_cast<float>(rel);
        env = relStart * std::max(0.f, 1.f - r);
      }
      float l = 0.f, r = 0.f;
      for (int u = 0; u < U; ++u) {
        const std::size_t k = static_cast<std::size_t>(u);
        double& ph = phase[k];
        const double dt = inc[k];
        float s = 0.f;
        if (pr.sine > 0) s += pr.sine * static_cast<float>(std::sin(2.0 * kPi * ph));
        if (pr.tri > 0) s += pr.tri * static_cast<float>(4.0 * std::fabs(ph - 0.5) - 1.0);
        if (pr.saw > 0) s += pr.saw * static_cast<float>(2.0 * ph - 1.0 - polyBlep(ph, dt));
        if (pr.square > 0) { double q = ph < 0.5 ? 1.0 : -1.0; q += polyBlep(ph, dt); q -= polyBlep(std::fmod(ph + 0.5, 1.0), dt); s += pr.square * static_cast<float>(q); }
        ph += dt; if (ph >= 1.0) ph -= 1.0;
        l += s * vl[k]; r += s * vr[k];
      }
      if (pr.subLevel > 0) {
        const float sub = pr.subLevel * static_cast<float>(std::sin(2.0 * kPi * subPhase));
        subPhase += f0 * 0.5 / sr; if (subPhase >= 1.0) subPhase -= 1.0;
        l += sub; r += sub;
      }
      // 24 dB/oct low-pass (two one-poles each side), cutoff opened by the envelope
      const float cutoff = std::clamp(pr.cutoffHz + pr.envToCutoff * env * (0.4f + 0.6f * vel), 40.f, static_cast<float>(sr) * 0.45f);
      const float a = 1.f - std::exp(-2.f * static_cast<float>(kPi) * cutoff / static_cast<float>(sr));
      lpA += a * (l - lpA); lpB += a * (lpA - lpB);
      lpC += a * (r - lpC); lpD += a * (lpC - lpD);
      const float g = env * vel * master;
      out.channel(0)[s0 + i] += lpB * g * lp * 1.41f;
      out.channel(1)[s0 + i] += lpD * g * rp * 1.41f;
    }
  }
  return out;
}

AudioBuffer renderDrums(const MidiClip& clip, double bpm, double sr) {
  double endBeat = 0;
  for (auto& n : clip.notes) endBeat = std::max(endBeat, n.start + n.length);
  const std::size_t total = framesFor(endBeat, bpm, sr) + static_cast<std::size_t>(2.0 * sr);
  AudioBuffer out(2, total);
  Noise noise;
  for (const Note& n : clip.notes) {
    const std::size_t s0 = framesFor(n.start, bpm, sr);
    const float vel = std::pow(static_cast<float>(n.velocity) / 127.f, 1.3f);
    float pan = 0.f;
    double dur = 0.3;
    switch (n.pitch) {
      case 36: dur = 0.5; break;
      case 38: dur = 0.35; break;
      case 39: dur = 0.3; pan = 0.1f; break;
      case 42: dur = 0.08; pan = 0.25f; break;
      case 46: dur = 0.4; pan = 0.25f; break;
      case 45: dur = 0.5; pan = -0.3f; break;
      case 49: dur = 1.8; pan = -0.4f; break;
      case 51: dur = 0.9; pan = 0.35f; break;
      default: dur = 0.2; break;
    }
    const std::size_t len = static_cast<std::size_t>(dur * sr);
    double ph = 0.0, ph2 = 0.0;
    float hp1 = 0.f, prevN = 0.f, bp = 0.f;
    const float gl = panL(pan), gr = panR(pan);
    for (std::size_t i = 0; i < len && s0 + i < total; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sr);
      float s = 0.f;
      const float nz = noise.next();
      const float hpn = nz - prevN; prevN = nz;  // crude high-pass: differentiated noise
      switch (n.pitch) {
        case 36: {  // kick: falling sine plus a short click
          const double f = 45.0 + 110.0 * std::exp(-t * 28.0);
          ph += f / sr; s = static_cast<float>(std::sin(2.0 * kPi * ph)) * std::exp(-t * 7.0f) + 0.25f * nz * std::exp(-t * 220.f);
          s *= 1.1f; break;
        }
        case 38: {  // snare: body tone plus noise
          ph += 190.0 / sr; s = 0.5f * static_cast<float>(std::sin(2.0 * kPi * ph)) * std::exp(-t * 28.f) + 0.55f * hpn * std::exp(-t * 16.f); break;
        }
        case 39: {  // clap: three quick noise bursts
          const float b = (t < 0.01f ? 1.f : t < 0.02f ? 0.2f : t < 0.03f ? 1.f : t < 0.04f ? 0.2f : 1.f) * std::exp(-t * 18.f);
          hp1 += 0.45f * (nz - hp1); s = 0.6f * (hp1) * b * 2.f; break;
        }
        case 42: s = 0.45f * hpn * std::exp(-t * 70.f); break;
        case 46: s = 0.4f * hpn * std::exp(-t * 9.f); break;
        case 45: { const double f = 80.0 + 70.0 * std::exp(-t * 14.0); ph += f / sr; s = static_cast<float>(std::sin(2.0 * kPi * ph)) * std::exp(-t * 7.f); break; }
        case 49: s = 0.4f * hpn * std::exp(-t * 2.2f); break;
        case 51: {
          ph += 5200.0 / sr; ph2 += 3100.0 / sr;
          s = 0.25f * hpn * std::exp(-t * 6.f) + 0.12f * (static_cast<float>(std::sin(2.0 * kPi * ph)) + static_cast<float>(std::sin(2.0 * kPi * ph2))) * std::exp(-t * 7.f); break;
        }
        default: bp += 0.1f * (nz - bp); s = bp * std::exp(-t * 20.f); break;
      }
      s *= vel * 0.8f;
      out.channel(0)[s0 + i] += s * gl * 1.41f;
      out.channel(1)[s0 + i] += s * gr * 1.41f;
    }
  }
  return out;
}

}  // namespace wezt::music
