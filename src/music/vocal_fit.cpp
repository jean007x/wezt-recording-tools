#include "music/vocal_fit.h"

#include <algorithm>
#include <cmath>

#include "music/audio_to_midi.h"
#include "music/psola.h"

namespace wezt::music {
namespace {

using Vec = std::vector<float>;

// Retunes one segment by `semitones` and fits it into `outLen` frames (at most maxStretch), keeping the voice's formants.
Vec shiftAndFit(const Vec& seg, double sr, double semitones, std::size_t outLen, double maxStretch) {
  const double want = static_cast<double>(outLen) / static_cast<double>(seg.size());
  const double used = std::clamp(want, 1.0 / maxStretch, maxStretch);
  const std::size_t n = static_cast<std::size_t>(static_cast<double>(seg.size()) * used);
  Vec y = psola(seg, sr, std::pow(2.0, semitones / 12.0), n);
  if (y.size() > outLen) y.resize(outLen);
  return y;
}

void addFaded(Vec& dst, const Vec& src, std::size_t at, float gain, std::size_t fade) {
  for (std::size_t i = 0; i < src.size() && at + i < dst.size(); ++i) {
    float g = gain;
    if (i < fade) g *= static_cast<float>(i) / static_cast<float>(fade);
    if (src.size() - i <= fade) g *= static_cast<float>(src.size() - i) / static_cast<float>(fade);
    dst[at + i] += src[i] * g;
  }
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
  return v[v.size() / 2];
}

struct Src { Vec audio; double midi = 0; std::size_t start = 0; };

std::vector<Src> sungSegments(const AudioBuffer& vocal, double sr, const Vec& mono, std::string& why) {
  std::vector<Src> src;
  // find the sung notes (seconds: tempo 60 makes one beat one second)
  AudioToMidiParams ap; ap.bpm = 60; ap.minNoteMs = 90;
  const MidiClip sung = audioToMidi(vocal, sr, ap);
  if (sung.notes.empty()) { why = "I could not find sung notes in the recording. Sing clearly, one syllable per note, close to the microphone."; return src; }
  const auto frames = trackPitch(vocal, sr);
  for (const Note& n : sung.notes) {
    const std::size_t a = static_cast<std::size_t>(std::max(0.0, n.start) * sr);
    const std::size_t b = std::min(mono.size(), static_cast<std::size_t>((n.start + n.length) * sr));
    if (b <= a + 64) continue;
    std::vector<double> hz;
    for (const auto& f : frames) if (f.hz > 0 && f.confidence > 0.5 && f.time >= n.start && f.time <= n.start + n.length) hz.push_back(f.hz);
    const double m = hz.empty() ? n.pitch : 69.0 + 12.0 * std::log2(median(hz) / 440.0);
    src.push_back({Vec(mono.begin() + static_cast<long>(a), mono.begin() + static_cast<long>(b)), m, a});
  }
  if (src.empty()) why = "The sung notes were too short to use.";
  return src;
}

Vec monoOf(const AudioBuffer& v) {
  Vec mono(v.frames, 0.f);
  for (std::size_t c = 0; c < v.channels; ++c) for (std::size_t i = 0; i < v.frames; ++i) mono[i] += v.channel(c)[i] / static_cast<float>(v.channels);
  return mono;
}

// Semitones from `midi` to the note `steps` scale steps above it (negative = below), in key k.
int scaleInterval(const Key& k, double midi, int steps) {
  const auto sc = scaleIntervals(k.mode);
  const int m = static_cast<int>(std::lround(midi));
  const int rel = ((m - k.root) % 12 + 12) % 12;
  int deg = 0, bestD = 99;
  for (int d = 0; d < 7; ++d) { const int dist = std::abs(sc[static_cast<std::size_t>(d)] - rel); if (dist < bestD) { bestD = dist; deg = d; } }
  const int idx = deg + steps;
  const int oct = static_cast<int>(std::floor(static_cast<double>(idx) / 7.0));
  const int within = ((idx % 7) + 7) % 7;
  return 12 * oct + sc[static_cast<std::size_t>(within)] - sc[static_cast<std::size_t>(deg)];
}


}  // namespace

VocalFit fitVocal(const AudioBuffer& vocal, double sr, const MidiClip& melody, const VocalFitParams& p) {
  VocalFit r;
  if (vocal.frames == 0 || vocal.channels == 0) { r.message = "The vocal recording is empty."; return r; }
  if (melody.notes.empty()) { r.message = "The melody has no notes."; return r; }
  if (p.bpm <= 0) { r.message = "The tempo must be above zero."; return r; }
  const Vec mono = monoOf(vocal);

  std::string why;
  const std::vector<Src> src = sungSegments(vocal, sr, mono, why);
  if (src.empty()) { r.message = why; return r; }

  // 2. place them on the melody
  const double secPerBeat = 60.0 / p.bpm;
  const std::size_t total = static_cast<std::size_t>(melody.end() * secPerBeat * sr) + static_cast<std::size_t>(0.5 * sr);
  Vec lead(total, 0.f), harm(total, 0.f);
  const std::size_t win = std::max<std::size_t>(256, static_cast<std::size_t>(0.025 * sr));  // shortest note
  const std::size_t fade = std::max<std::size_t>(16, static_cast<std::size_t>(0.008 * sr));
  const auto scale = scaleIntervals(p.key.mode);
  const std::size_t M = melody.notes.size(), N = src.size();
  const float hg = std::pow(10.f, static_cast<float>(p.harmonyDb) / 20.f);
  for (std::size_t j = 0; j < M; ++j) {
    const Note& t = melody.notes[j];
    const Src& s = src[std::min(N - 1, j * N / M)];
    double shift = t.pitch - s.midi;
    shift -= 12.0 * std::floor((shift + 6.0) / 12.0);  // fold into -6..6: stay in the singer's own register
    const std::size_t at = static_cast<std::size_t>(t.start * secPerBeat * sr);
    const std::size_t len = std::max<std::size_t>(win, static_cast<std::size_t>(t.length * secPerBeat * sr * 0.96));
    const float vel = 0.55f + 0.45f * static_cast<float>(t.velocity) / 127.f;
    addFaded(lead, shiftAndFit(s.audio, sr, shift, len, p.maxStretch), at, vel, fade);
    if (p.harmony) {
      // diatonic third above the target note, in the key
      const int rel = ((t.pitch - p.key.root) % 12 + 12) % 12;
      int deg = 0, bestD = 99;
      for (int d = 0; d < 7; ++d) { const int dist = std::abs(scale[static_cast<std::size_t>(d)] - rel); if (dist < bestD) { bestD = dist; deg = d; } }
      const int up = scale[static_cast<std::size_t>((deg + 2) % 7)] + ((deg + 2) >= 7 ? 12 : 0) - scale[static_cast<std::size_t>(deg)];
      addFaded(harm, shiftAndFit(s.audio, sr, shift + up, len, p.maxStretch), at, vel * hg, fade);
    }
  }
  r.lead = AudioBuffer(1, total);
  std::copy(lead.begin(), lead.end(), r.lead.channel(0));
  if (p.harmony) { r.harmony = AudioBuffer(1, total); std::copy(harm.begin(), harm.end(), r.harmony.channel(0)); }
  r.sourceNotes = static_cast<int>(N);
  r.targetNotes = static_cast<int>(M);
  r.ok = true;
  r.message = "Fitted " + std::to_string(N) + " sung notes to " + std::to_string(M) + " melody notes.";
  if (N != M) r.message += " (You sang " + std::to_string(N) + " notes; for the most natural result sing one syllable per melody note.)";
  return r;
}

AudioBuffer makeChoir(const AudioBuffer& vocal, double sr, const ChoirParams& p) {
  if (vocal.frames == 0 || vocal.channels == 0) return {};
  const Vec mono = monoOf(vocal);
  std::string why;
  const auto src = sungSegments(vocal, sr, mono, why);
  if (src.empty()) return {};
  static const int kSteps[] = {2, 4, -3, 7, -5, 9, -7, 11};
  const int voices = std::clamp(p.voices, 1, 8);
  AudioBuffer out(2, vocal.frames + static_cast<std::size_t>(0.2 * sr));
  Rng rng(7);
  const std::size_t fade = std::max<std::size_t>(16, static_cast<std::size_t>(0.008 * sr));
  for (int v = 0; v < voices; ++v) {
    Vec line(out.frames, 0.f);
    const double detune = (static_cast<double>(rng.next() % 2001) / 1000.0 - 1.0) * p.detuneCents / 100.0;   // semitones
    const std::size_t delay = static_cast<std::size_t>((static_cast<double>(v + 1) / voices) * p.spreadMs * 0.001 * sr);
    for (const Src& s : src) {
      const int up = scaleInterval(p.key, s.midi, kSteps[v]);
      const Vec g = psola(s.audio, sr, std::pow(2.0, (up + detune) / 12.0), s.audio.size());
      addFaded(line, g, s.start + delay, 1.f, fade);
    }
    const float pan = voices == 1 ? 0.f : -1.f + 2.f * static_cast<float>(v) / static_cast<float>(voices - 1);
    const float gl = std::cos((pan + 1.f) * 0.785398f), gr = std::sin((pan + 1.f) * 0.785398f);
    const float gain = std::pow(10.f, p.voiceDb / 20.f);
    for (std::size_t i = 0; i < out.frames; ++i) { out.channel(0)[i] += line[i] * gain * gl; out.channel(1)[i] += line[i] * gain * gr; }
  }
  return out;
}

}  // namespace wezt::music
