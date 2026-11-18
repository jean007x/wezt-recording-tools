#include "music/audio_to_midi.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "ai/fft.h"

namespace wezt::music {
namespace {

std::vector<float> mono(const AudioBuffer& a) {
  std::vector<float> m(a.frames, 0.f);
  if (a.channels == 0) return m;
  for (std::size_t c = 0; c < a.channels; ++c) for (std::size_t i = 0; i < a.frames; ++i) m[i] += a.channel(c)[i] / static_cast<float>(a.channels);
  return m;
}

double toBeats(double sec, double bpm) { return sec * bpm / 60.0; }

}  // namespace

std::vector<PitchFrame> trackPitch(const AudioBuffer& audio, double sr, double minHz, double maxHz) {
  std::vector<PitchFrame> out;
  const auto x = mono(audio);
  const std::size_t W = 1024, hop = 512;
  const std::size_t tauMax = std::min<std::size_t>(static_cast<std::size_t>(sr / minHz) + 1, W);
  const std::size_t tauMin = std::max<std::size_t>(2, static_cast<std::size_t>(sr / maxHz));
  if (x.size() < W + tauMax) return out;
  std::vector<double> d(tauMax + 1), cm(tauMax + 1);
  for (std::size_t pos = 0; pos + W + tauMax < x.size(); pos += hop) {
    PitchFrame f;
    f.time = static_cast<double>(pos + W / 2) / sr;
    double energy = 0;
    for (std::size_t j = 0; j < W; ++j) energy += static_cast<double>(x[pos + j]) * x[pos + j];
    if (energy / static_cast<double>(W) < 1e-8) { out.push_back(f); continue; }
    for (std::size_t tau = 1; tau <= tauMax; ++tau) {
      double s = 0;
      for (std::size_t j = 0; j < W; ++j) { const double df = static_cast<double>(x[pos + j]) - x[pos + j + tau]; s += df * df; }
      d[tau] = s;
    }
    double run = 0;
    cm[0] = 1;
    for (std::size_t tau = 1; tau <= tauMax; ++tau) { run += d[tau]; cm[tau] = run > 0 ? d[tau] * static_cast<double>(tau) / run : 1.0; }
    std::size_t best = 0;
    for (std::size_t tau = tauMin; tau < tauMax; ++tau)
      if (cm[tau] < 0.15) {
        while (tau + 1 < tauMax && cm[tau + 1] < cm[tau]) ++tau;
        best = tau; break;
      }
    if (best == 0) { out.push_back(f); continue; }
    double t = static_cast<double>(best);
    if (best > 1 && best + 1 <= tauMax) {  // parabolic interpolation
      const double a = cm[best - 1], b = cm[best], c = cm[best + 1];
      const double den = a - 2 * b + c;
      if (std::fabs(den) > 1e-12) t += 0.5 * (a - c) / den;
    }
    f.hz = sr / t;
    f.confidence = std::clamp(1.0 - cm[best], 0.0, 1.0);
    out.push_back(f);
  }
  return out;
}

MidiClip audioToMidi(const AudioBuffer& audio, double sr, const AudioToMidiParams& p) {
  MidiClip clip;
  clip.name = "Audio to MIDI";
  const auto frames = trackPitch(audio, sr);
  if (frames.empty()) return clip;
  // frame loudness relative to the loudest frame, for the gate and for velocity
  const auto x = mono(audio);
  std::vector<double> rms(frames.size(), 0);
  double maxRms = 1e-9;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const std::size_t c = static_cast<std::size_t>(frames[i].time * sr), a = c > 512 ? c - 512 : 0, b = std::min(x.size(), c + 512);
    double s = 0; for (std::size_t j = a; j < b; ++j) s += static_cast<double>(x[j]) * x[j];
    rms[i] = std::sqrt(s / std::max<std::size_t>(1, b - a));
    maxRms = std::max(maxRms, rms[i]);
  }
  const double gate = std::pow(10.0, p.gateDb / 20.0) * maxRms;
  // midi pitch per frame (fractional), NaN when unvoiced or gated
  std::vector<double> mp(frames.size(), -1);
  for (std::size_t i = 0; i < frames.size(); ++i)
    if (frames[i].hz > 0 && frames[i].confidence > 0.6 && rms[i] > gate && rms[i] > 0.0)
      mp[i] = 69.0 + 12.0 * std::log2(frames[i].hz / 440.0);
  // a one-frame glitch inside a note is bridged; isolated single frames are dropped
  for (std::size_t i = 1; i + 1 < mp.size(); ++i) {
    if (mp[i] < 0 && mp[i - 1] >= 0 && mp[i + 1] >= 0 && std::fabs(mp[i - 1] - mp[i + 1]) < 0.8) mp[i] = 0.5 * (mp[i - 1] + mp[i + 1]);
    else if (mp[i] >= 0 && mp[i - 1] < 0 && mp[i + 1] < 0) mp[i] = -1;
  }
  struct Seg { std::size_t a, b; std::vector<double> v; double peak; };
  std::vector<Seg> segs;
  Seg cur{0, 0, {}, 0};
  bool open = false;
  auto close = [&](std::size_t end) { if (open) { cur.b = end; segs.push_back(cur); open = false; } };
  for (std::size_t i = 0; i < mp.size(); ++i) {
    if (mp[i] < 0) { close(i); continue; }
    if (open) {
      std::vector<double> tmp = cur.v;
      std::nth_element(tmp.begin(), tmp.begin() + static_cast<long>(tmp.size() / 2), tmp.end());
      const double med = tmp[tmp.size() / 2];
      if (std::fabs(mp[i] - med) > 0.75) close(i);   // a new note starts on a pitch jump
    }
    if (!open) { cur = Seg{i, i, {}, 0}; open = true; }
    cur.v.push_back(mp[i]);
    cur.peak = std::max(cur.peak, rms[i]);
  }
  close(mp.size());
  for (auto& s : segs) {
    const double t0 = frames[s.a].time - 512.0 / sr, t1 = frames[std::min(s.b, frames.size()) - 1].time + 512.0 / sr;
    if ((t1 - t0) * 1000.0 < p.minNoteMs) continue;
    std::vector<double> v = s.v;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
    int pitch = static_cast<int>(std::lround(v[v.size() / 2]));
    if (p.snapToKey) {
      const auto iv = scaleIntervals(p.key.mode);
      int bestP = pitch, bestD = 99;
      for (int c = pitch - 2; c <= pitch + 2; ++c) {
        const int rel = (((c - p.key.root) % 12) + 12) % 12;
        if (std::find(iv.begin(), iv.end(), rel) != iv.end() && std::abs(c - pitch) < bestD) { bestD = std::abs(c - pitch); bestP = c; }
      }
      pitch = bestP;
    }
    if (pitch < 0 || pitch > 127) continue;
    Note n;
    n.pitch = pitch;
    n.start = toBeats(std::max(0.0, t0), p.bpm);
    n.length = toBeats(t1 - std::max(0.0, t0), p.bpm);
    n.velocity = std::clamp(static_cast<int>(40 + 87 * std::sqrt(s.peak / maxRms)), 1, 127);
    if (p.gridBeats > 0) {
      const double g = p.gridBeats;
      n.start = std::round(n.start / g) * g;
      n.length = std::max(g, std::round(n.length / g) * g);
    }
    // monophonic: a note never overlaps the next
    if (!clip.notes.empty()) {
      Note& prev = clip.notes.back();
      if (n.start < prev.start + 1e-9) n.start = prev.start + p.gridBeats;
      if (prev.start + prev.length > n.start) prev.length = std::max(1e-3, n.start - prev.start);
    }
    clip.notes.push_back(n);
  }
  clip.lengthBeats = toBeats(static_cast<double>(audio.frames) / sr, p.bpm);
  return clip;
}

MidiClip beatboxToDrums(const AudioBuffer& audio, double sr, const AudioToMidiParams& p) {
  MidiClip clip;
  clip.name = "Beatbox";
  clip.channel = 9;
  const auto x = mono(audio);
  if (x.size() < 2048) return clip;
  const std::size_t hop = static_cast<std::size_t>(0.005 * sr);
  std::vector<double> env;
  for (std::size_t i = 0; i + hop <= x.size(); i += hop) {
    double s = 0; for (std::size_t j = 0; j < hop; ++j) s += static_cast<double>(x[i + j]) * x[i + j];
    env.push_back(std::sqrt(s / static_cast<double>(hop)));
  }
  const double mx = *std::max_element(env.begin(), env.end());
  if (mx < 1e-6) return clip;
  const double gate = std::pow(10.0, p.gateDb / 20.0) * mx;
  ai::FFT fft(1024);
  std::vector<std::complex<double>> buf(1024);
  const std::size_t minGap = static_cast<std::size_t>(0.07 * sr / static_cast<double>(hop));
  std::size_t lastOnset = 0;
  bool any = false;
  for (std::size_t i = 4; i + 1 < env.size(); ++i) {
    double prev = 0; for (std::size_t k = 1; k <= 4; ++k) prev += env[i - k];
    prev /= 4.0;
    if (env[i] > gate && env[i] > 2.0 * prev + 1e-6 && env[i] >= env[i + 1] * 0.5 && (!any || i - lastOnset >= minGap)) {
      // analyse the 1024 samples starting at the onset
      const std::size_t s0 = i * hop;
      if (s0 + 1024 > x.size()) break;
      for (std::size_t j = 0; j < 1024; ++j) buf[j] = x[s0 + j] * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(j) / 1023.0));
      fft.forward(buf);
      double tot = 0, low = 0, high = 0, cen = 0;
      for (std::size_t k = 1; k < 512; ++k) {
        const double hz = static_cast<double>(k) * sr / 1024.0, e = std::norm(buf[k]);
        tot += e; cen += e * hz;
        if (hz < 250) low += e;
        if (hz > 5000) high += e;
      }
      if (tot <= 0) continue;
      const double lowR = low / tot, highR = high / tot, centroid = cen / tot;
      int drum = Snare;
      if (lowR > 0.5 || centroid < 600) drum = Kick;
      else if (highR > 0.75) drum = ClosedHat;   // hats are almost all top end; snares keep much of their energy below 5 kHz
      Note n;
      n.pitch = drum;
      n.start = toBeats(static_cast<double>(s0) / sr, p.bpm);
      n.length = 0.25;
      n.velocity = std::clamp(static_cast<int>(35 + 92 * std::sqrt(env[i] / mx)), 1, 127);
      if (p.gridBeats > 0) n.start = std::round(n.start / p.gridBeats) * p.gridBeats;
      clip.notes.push_back(n);
      lastOnset = i; any = true;
    }
  }
  clip.lengthBeats = toBeats(static_cast<double>(audio.frames) / sr, p.bpm);
  return clip;
}

}  // namespace wezt::music
