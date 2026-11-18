#include "music/vocal_fit.h"
// Music engine: theory, progressions, melody, session players, MIDI files, synth, whole songs,
// audio-to-MIDI and stem separation.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

#include "ai/audio_file.h"
#include "ai/features.h"
#include "io/wav.h"
#include "music/audio_to_midi.h"
#include "music/separation.h"
#include "music/song.h"
#include "session/session.h"

using namespace wezt;
using namespace wezt::music;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("FAIL %s:%d %s=%g vs %g\n", __FILE__, __LINE__, #a, _a, _b); ++failures; } } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846;

void testTheory() {
  Key k;
  CHECK(parseKey("C", k) && k.root == 0 && k.mode == Mode::Major);
  CHECK(parseKey("F# minor", k) && k.root == 6 && k.mode == Mode::Minor);
  CHECK(parseKey("Bb dorian", k) && k.root == 10 && k.mode == Mode::Dorian);
  CHECK(parseKey("Am", k) && k.root == 9 && k.mode == Mode::Minor);
  CHECK(!parseKey("H", k) && !parseKey("C banana", k));
  Key c{0, Mode::Major};
  CHECK(diatonicChord(c, 0, 0).name() == "C");
  CHECK(diatonicChord(c, 1, 0).name() == "Dm");
  CHECK(diatonicChord(c, 4, 1).name() == "G7");
  CHECK(diatonicChord(c, 0, 1).name() == "Cmaj7");
  CHECK(diatonicChord(c, 6, 0).name() == "Bdim");
  CHECK(diatonicChord(c, 1, 2).name() == "Dm9");
  Key a{9, Mode::Minor};
  CHECK(diatonicChord(a, 0, 1).name() == "Am7");
  CHECK(diatonicChord(a, 5, 0).name() == "F");
  CHECK(degreePitchClass(a, 2) == 0);  // C is the third of A minor
}

void testProgression() {
  ProgressionParams p;
  p.key = {0, Mode::Major}; p.bars = 8; p.seed = 5;
  const auto a = generateProgression(p);
  const auto b = generateProgression(p);
  CHECK(a.size() == 8);
  bool same = true;
  for (std::size_t i = 0; i < a.size(); ++i) same = same && a[i].name() == b[i].name();
  CHECK(same);                                   // deterministic
  CHECK(a.front().degree == 0 && a.back().degree == 0);   // starts and ends on the tonic
  std::set<std::string> diffSeeds;
  for (int s = 1; s <= 12; ++s) { p.seed = static_cast<std::uint64_t>(s); std::string t; for (auto& c : generateProgression(p)) t += c.name() + " "; diffSeeds.insert(t); }
  CHECK(diffSeeds.size() >= 4);                  // seeds give variety
  // every chord is diatonic to the key
  p.key = {7, Mode::Minor};
  const auto iv = scaleIntervals(Mode::Minor);
  for (std::uint64_t s = 1; s <= 10; ++s) {
    p.seed = s;
    for (auto& c : generateProgression(p))
      for (int x : c.intervals) {
        const int rel = (((c.root + x) - p.key.root) % 12 + 12) % 12;
        CHECK(std::find(iv.begin(), iv.end(), rel) != iv.end());
      }
  }
  // richer styles use extensions
  ProgressionParams j; j.style = "jazz"; j.mood = "dreamy"; j.bars = 4;
  CHECK(generateProgression(j)[0].intervals.size() >= 5);
  ProgressionParams pop; pop.style = "pop"; pop.mood = "happy"; pop.bars = 4;
  CHECK(generateProgression(pop)[0].intervals.size() == 3);
  // a learned model pulls the output toward what it has seen
  ChordModel m;
  for (int i = 0; i < 20; ++i) m.learn({0, 5, 1, 4, 0});
  CHECK(m.trained());
  ChordModel m2;
  CHECK(m2.load(m.save()) && m2.trained());
  CHECK(!m2.load("9 9 1"));
}

void testMelodyAndParts() {
  Key key{0, Mode::Major};
  ProgressionParams pp; pp.key = key; pp.bars = 8; pp.seed = 3;
  const auto ch = generateProgression(pp);
  MelodyParams mp; mp.key = key; mp.seed = 4;
  const auto mel = generateMelody(ch, mp);
  CHECK(mel.notes.size() >= 12);
  const auto iv = scaleIntervals(key.mode);
  int strongChordTones = 0, strong = 0;
  for (auto& n : mel.notes) {
    CHECK(n.pitch >= mp.lowPitch && n.pitch <= mp.highPitch);
    CHECK(std::find(iv.begin(), iv.end(), ((n.pitch - key.root) % 12 + 12) % 12) != iv.end());   // always in key
    CHECK(n.start >= 0 && n.start + n.length <= 32.0 + 1e-6 && n.length > 0);
    if (std::fmod(n.start, 4.0) == 0.0) {
      ++strong;
      double t = 0; const Chord* c = &ch[0];
      for (auto& cc : ch) { if (n.start < t + cc.beats) { c = &cc; break; } t += cc.beats; }
      for (int x : c->intervals) if (((c->root + x) % 12) == n.pitch % 12) { ++strongChordTones; break; }
    }
  }
  CHECK(strong > 0 && strongChordTones >= strong * 7 / 10);   // downbeats mostly land on chord tones
  // the last note is the tonic
  CHECK(mel.notes.back().pitch % 12 == key.root);
  // same seed, same melody; different seed, different melody
  const auto mel2 = generateMelody(ch, mp);
  CHECK(mel2.notes.size() == mel.notes.size() && mel2.notes[3].pitch == mel.notes[3].pitch);
  mp.seed = 99;
  const auto mel3 = generateMelody(ch, mp);
  bool differs = mel3.notes.size() != mel.notes.size();
  for (std::size_t i = 0; !differs && i < mel.notes.size(); ++i) differs = mel3.notes[i].pitch != mel.notes[i].pitch || mel3.notes[i].start != mel.notes[i].start;
  CHECK(differs);

  PartParams part; part.key = key; part.style = "pop";
  const auto bass = bassPart(ch, part);
  CHECK(!bass.notes.empty());
  for (auto& n : bass.notes) CHECK(n.pitch >= 24 && n.pitch <= 52);
  // bass plays the chord root on the first beat of each chord
  double t = 0;
  for (auto& c : ch) {
    bool rootOnOne = false;
    for (auto& n : bass.notes) if (std::fabs(n.start - t) < 1e-9 && n.pitch % 12 == c.root) rootOnOne = true;
    CHECK(rootOnOne);
    t += c.beats;
  }
  const auto keys = keysPart(ch, part);
  CHECK(!keys.notes.empty());
  for (auto& n : keys.notes) CHECK(n.pitch >= 40 && n.pitch <= 90);
  const auto pad = padPart(ch, part);
  CHECK(pad.notes.size() >= ch.size() * 3);
  // voice leading: the pad does not jump around
  std::vector<double> centres;
  for (std::size_t i = 0, k = 0; i < ch.size(); ++i) { double s = 0; int n = 0; while (k < pad.notes.size() && pad.notes[k].start < (i + 1) * 4.0 - 1e-9) { s += pad.notes[k].pitch; ++n; ++k; } centres.push_back(s / std::max(1, n)); }
  for (std::size_t i = 1; i < centres.size(); ++i) CHECK(std::fabs(centres[i] - centres[i - 1]) < 7.0);
  for (const char* style : {"pop", "rock", "lofi", "edm", "jazz", "cinematic", "rnb", "folk", "trap"}) {
    part.style = style;
    const auto d = drumPart(8, part);
    CHECK(d.channel == 9 && d.notes.size() >= 8);
    for (auto& n : d.notes) CHECK(n.start >= 0 && n.start < 32.0);
  }
  part.style = "edm";
  int kicks = 0;
  for (auto& n : drumPart(4, part, false).notes) if (n.pitch == Kick) ++kicks;
  CHECK(kicks == 16);   // four on the floor
}

void testMidiFile() {
  MidiClip a; a.name = "lead";
  a.notes = {{60, 0, 1, 100}, {64, 1, 0.5, 80}, {67, 1.5, 2.25, 64}};
  MidiClip d; d.name = "drums"; d.channel = 9; d.notes = {{36, 0, 0.25, 110}, {38, 1, 0.25, 100}};
  const auto bytes = writeMidiFile({a, d}, 96.5, 4, {0, 4});
  MidiFile f; std::string err;
  CHECK(readMidiFile(bytes, f, &err));
  NEAR(f.bpm, 96.5, 0.01);
  CHECK(f.clips.size() == 2);
  if (f.clips.size() == 2) {
    CHECK(f.clips[0].name == "lead" && f.clips[0].notes.size() == 3);
    NEAR(f.clips[0].notes[2].length, 2.25, 1e-3);
    CHECK(f.clips[0].notes[1].pitch == 64 && f.clips[0].notes[1].velocity == 80);
    CHECK(f.clips[1].channel == 9 && f.clips[1].notes[1].start > 4.9);   // the 4-beat offset
  }
  CHECK(!readMidiFile({1, 2, 3}, f, &err));
  auto trunc = bytes; trunc.resize(trunc.size() - 9);
  CHECK(!readMidiFile(trunc, f, &err));
  std::vector<std::uint8_t> junk(bytes.begin(), bytes.begin() + 30);
  readMidiFile(junk, f, &err);   // must not crash
}

void testSynth() {
  const double sr = 44100;
  MidiClip c; c.notes = {{57, 0, 1, 100}, {64, 1, 1, 100}};
  for (const auto& name : synthPresetNames()) {
    const auto b = renderSynth(c, synthPreset(name), 120, sr);
    float pk = 0; bool finite = true;
    for (std::size_t ch = 0; ch < 2; ++ch) for (std::size_t i = 0; i < b.frames; ++i) { pk = std::max(pk, std::fabs(b.channel(ch)[i])); finite = finite && std::isfinite(b.channel(ch)[i]); }
    CHECK(finite && pk > 0.02f && pk < 1.5f);
  }
  // the pitch is right: a plain sine preset at A4
  SynthPreset sine = synthPreset("keys");
  sine.sine = 1; sine.tri = 0; sine.saw = 0; sine.square = 0; sine.unison = 1; sine.cutoffHz = 12000; sine.envToCutoff = 0; sine.attack = 0.001f; sine.sustain = 1; sine.subLevel = 0;
  MidiClip a4; a4.notes = {{69, 0, 2, 110}};
  const auto b = renderSynth(a4, sine, 120, sr);
  const auto fr = trackPitch(b, sr);
  int ok = 0, voiced = 0;
  for (auto& f : fr) if (f.time > 0.2 && f.time < 0.9 && f.hz > 0) { ++voiced; if (std::fabs(f.hz - 440.0) < 3.0) ++ok; }
  CHECK(voiced > 5 && ok == voiced);
  // drums: every drum sounds, kicks are low, hats are bright
  MidiClip dr; dr.channel = 9;
  dr.notes = {{36, 0, 0.25, 110}, {38, 1, 0.25, 110}, {42, 2, 0.25, 110}, {39, 3, 0.25, 110}, {49, 4, 0.25, 110}, {51, 6, 0.25, 110}, {45, 7, 0.25, 110}, {46, 8, 0.25, 110}};
  const auto d = renderDrums(dr, 120, sr);
  for (int i = 0; i < 8; ++i) {
    double e = 0;
    const std::size_t a0 = static_cast<std::size_t>(dr.notes[static_cast<std::size_t>(i)].start * 0.5 * sr);
    for (std::size_t k = a0; k < a0 + static_cast<std::size_t>(0.2 * sr) && k < d.frames; ++k) e += d.channel(0)[k] * d.channel(0)[k];
    CHECK(e > 1e-3);
  }
}

void testSong() {
  SongSpec spec; spec.title = "Test Song"; spec.style = "pop"; spec.mood = "happy"; spec.seed = 7;
  spec.sections = {{"intro", "intro", 2, -1}, {"verse", "verse", 4, -1}, {"chorus", "chorus", 4, -1}};
  const Song song = composeSong(spec);
  CHECK(song.beats == 40.0 && song.layout.size() == 3);
  NEAR(song.bpm, 110, 1e-9);
  CHECK(song.tracks.size() == 5);
  CHECK(song.chords.size() == 10);
  CHECK(song.chartText().find("chorus") != std::string::npos);
  // the intro has no drums; the chorus has more going on than the verse
  int introDrums = 0, verseDrums = 0, chorusDrums = 0;
  for (auto& t : song.tracks) if (t.name == "Drums") for (auto& n : t.midi.notes) { if (n.start < 8) ++introDrums; else if (n.start < 24) ++verseDrums; else ++chorusDrums; }
  CHECK(introDrums == 0 && verseDrums > 0 && chorusDrums > 0);
  // same spec, same song
  const Song again = composeSong(spec);
  CHECK(again.tracks[1].midi.notes.size() == song.tracks[1].midi.notes.size() && again.chartText() == song.chartText());
  // mood without a given key picks the mode
  SongSpec sad; sad.mood = "sad";
  CHECK(composeSong(sad).key.mode == Mode::Minor);
  // parts filter
  SongSpec onlyDrums; onlyDrums.parts = {"drums"};
  CHECK(composeSong(onlyDrums).tracks.size() == 1);

  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "wezt_song_test";
  fs::remove_all(dir);
  ProjectFiles files; std::string err;
  CHECK(writeProject(song, dir.string(), 44100, files, &err));
  CHECK(files.stems.size() == 5 && fs::exists(files.midi) && fs::exists(files.mix) && fs::exists(files.session));
  AudioBuffer mix; double sr = 0;
  CHECK(ai::loadAudioFile(files.mix, mix, sr, &err) && sr == 44100);
  CHECK(mix.frames >= static_cast<std::size_t>(song.seconds() * 44100));
  const auto f = ai::analyze(mix, sr);
  CHECK(f.lufs > -40.f && f.lufs < -3.f && f.truePeakDb < 3.f);
  MidiFile mf;
  CHECK(loadMidiFile(files.midi, mf, &err) && mf.clips.size() == 5);
  NEAR(mf.bpm, 110, 0.01);
  auto loaded = session::Session::load(files.session, {}, &err);
  CHECK(loaded && loaded->trackIds().size() == 5 && loaded->memory().all().size() == 3);
  fs::remove_all(dir);
}

AudioBuffer hum(const std::vector<std::pair<double, double>>& notes /*midi, seconds*/, double sr, double vibratoCents = 0) {
  double total = 0; for (auto& n : notes) total += n.second + 0.1;
  AudioBuffer b(1, static_cast<std::size_t>(total * sr));
  double t = 0, ph = 0;
  for (auto& n : notes) {
    const std::size_t a = static_cast<std::size_t>(t * sr), len = static_cast<std::size_t>(n.second * sr);
    for (std::size_t i = 0; i < len; ++i) {
      const double tt = static_cast<double>(i) / sr;
      const double f = 440.0 * std::pow(2.0, (n.first - 69 + vibratoCents / 100.0 * std::sin(2 * kPi * 5.5 * tt)) / 12.0);
      ph += 2 * kPi * f / sr;
      const float env = std::min(1.0, tt * 60.0) * std::min(1.0, (n.second - tt) * 60.0);
      b.channel(0)[a + i] = 0.4f * env * (static_cast<float>(std::sin(ph)) + 0.4f * static_cast<float>(std::sin(2 * ph)));
    }
    t += n.second + 0.1;
  }
  return b;
}

void testAudioToMidi() {
  const double sr = 44100;
  const std::vector<std::pair<double, double>> tune = {{60, 0.5}, {64, 0.5}, {67, 0.5}, {72, 0.5}, {55, 0.5}};
  AudioToMidiParams p; p.bpm = 120;
  const auto m = audioToMidi(hum(tune, sr, 25), sr, p);   // with vibrato
  CHECK(m.notes.size() == 5);
  const int want[5] = {60, 64, 67, 72, 55};
  for (std::size_t i = 0; i < m.notes.size() && i < 5; ++i) CHECK(m.notes[i].pitch == want[i]);
  if (m.notes.size() == 5) { NEAR(m.notes[1].start, 1.2, 0.15); NEAR(m.notes[1].length, 1.0, 0.25); }
  // quantise and key snapping
  p.gridBeats = 0.5; p.snapToKey = true; p.key = {0, Mode::Major};
  const auto q = audioToMidi(hum({{61, 0.5}, {66, 0.5}}, sr), sr, p);   // out-of-key notes
  CHECK(q.notes.size() == 2);
  for (auto& n : q.notes) { CHECK(std::fmod(n.start, 0.5) < 1e-9); const int pc = n.pitch % 12; CHECK(pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11); }
  // silence and noise-free quiet input produce nothing, and tiny input is safe
  CHECK(audioToMidi(AudioBuffer(1, 44100), sr, {}).notes.empty());
  CHECK(audioToMidi(AudioBuffer(1, 10), sr, {}).notes.empty());

  // beatbox: a low thump, a noisy mid burst, a bright tick
  AudioBuffer bb(1, static_cast<std::size_t>(2.0 * sr));
  std::uint32_t s = 1;
  auto rnd = [&] { s = s * 1664525u + 1013904223u; return static_cast<float>(static_cast<std::int32_t>(s)) / 2147483648.f; };
  auto put = [&](double at, int kind) {
    const std::size_t a = static_cast<std::size_t>(at * sr);
    float prev = 0, lp = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.12 * sr); ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sr);
      const float n = rnd();
      float v = 0;
      if (kind == 0) v = 0.8f * std::sin(2.f * 3.14159f * (60.f + 80.f * std::exp(-t * 30.f)) * t) * std::exp(-t * 18.f);
      if (kind == 1) { lp += 0.25f * (n - lp); v = 0.9f * (0.6f * lp * 3.f + 0.3f * (n - prev)) * std::exp(-t * 22.f); prev = n; }
      if (kind == 2) { v = 0.5f * (n - prev) * std::exp(-t * 80.f); prev = n; }
      bb.channel(0)[a + i] += v;
    }
  };
  put(0.1, 0); put(0.6, 1); put(1.0, 2); put(1.4, 0);
  AudioToMidiParams bp; bp.bpm = 120;
  const auto dr = beatboxToDrums(bb, sr, bp);
  CHECK(dr.notes.size() == 4);
  if (dr.notes.size() == 4) {
    CHECK(dr.notes[0].pitch == Kick && dr.notes[1].pitch == Snare && dr.notes[2].pitch == ClosedHat && dr.notes[3].pitch == Kick);
    NEAR(dr.notes[1].start, 1.2, 0.1);   // 0.6 s at 120 bpm
  }
}

double energy(const AudioBuffer& b, std::size_t a, std::size_t e) { double s = 0; for (std::size_t c = 0; c < b.channels; ++c) for (std::size_t i = a; i < e && i < b.frames; ++i) s += double(b.channel(c)[i]) * b.channel(c)[i]; return s; }

void testSeparation() {
  const double sr = 44100;
  const std::size_t N = static_cast<std::size_t>(4.0 * sr);
  AudioBuffer mix(2, N);
  std::uint32_t seed = 7;
  auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(static_cast<std::int32_t>(seed)) / 2147483648.f; };
  AudioBuffer bass(2, N), click(2, N), vocal(2, N), guitar(2, N);
  for (std::size_t i = 0; i < N; ++i) {
    const double t = static_cast<double>(i) / sr;
    const float b = 0.3f * static_cast<float>(std::sin(2 * kPi * 55 * t));
    const float v = 0.25f * static_cast<float>(std::sin(2 * kPi * 440 * t) + 0.5 * std::sin(2 * kPi * 880 * t));
    const float g = 0.25f * static_cast<float>(std::sin(2 * kPi * 1318 * t));
    bass.channel(0)[i] = bass.channel(1)[i] = b;
    vocal.channel(0)[i] = vocal.channel(1)[i] = v;
    guitar.channel(0)[i] = g; guitar.channel(1)[i] = 0.f;     // hard left
    mix.channel(0)[i] = b + v + g; mix.channel(1)[i] = b + v;
  }
  for (double at = 0.25; at < 3.9; at += 0.5) {
    const std::size_t a = static_cast<std::size_t>(at * sr);
    for (std::size_t i = 0; i < 2500; ++i) { const float n = 0.6f * rnd() * std::exp(-static_cast<float>(i) / 350.f); click.channel(0)[a + i] = click.channel(1)[a + i] = n; mix.channel(0)[a + i] += n; mix.channel(1)[a + i] += n; }
  }
  StemSet st; std::string err;
  CHECK(separateDsp(mix, sr, st, &err));
  CHECK(st.drums.frames == N && st.bass.frames == N && st.vocals.frames == N && st.other.frames == N);
  // the stems add back to the input
  double errE = 0, inE = energy(mix, 0, N);
  for (std::size_t c = 0; c < 2; ++c) for (std::size_t i = 0; i < N; ++i) { const float sum = st.drums.channel(c)[i] + st.bass.channel(c)[i] + st.vocals.channel(c)[i] + st.other.channel(c)[i]; const double d = sum - mix.channel(c)[i]; errE += d * d; }
  CHECK(10 * std::log10(errE / inE + 1e-30) < -40);
  // each stem collects mostly its own source
  auto share = [&](const AudioBuffer& stem, const AudioBuffer& src) {   // energy of the stem explained by the source, via correlation
    double num = 0, den = 0; for (std::size_t c = 0; c < 2; ++c) for (std::size_t i = 0; i < N; ++i) { num += double(stem.channel(c)[i]) * src.channel(c)[i]; den += double(src.channel(c)[i]) * src.channel(c)[i]; }
    return num / den;   // ~1 when the source is fully in the stem
  };
  CHECK(share(st.bass, bass) > 0.7);
  CHECK(share(st.drums, click) > 0.5);
  CHECK(share(st.vocals, vocal) > 0.5);
  CHECK(share(st.drums, bass) < 0.2 && share(st.vocals, bass) < 0.2);       // little bass leaks into the others
  CHECK(share(st.other, guitar) > 0.5);                                      // the hard-left tone is not called a vocal
  CHECK(share(st.vocals, guitar) < 0.3);
  // refusals
  AudioBuffer tiny(2, 100);
  CHECK(!separateDsp(tiny, sr, st, &err));
  AudioBuffer five(5, 8192);
  CHECK(!separateDsp(five, sr, st, &err));
  SeparationOptions o; o.maxSeconds = 1;
  CHECK(!separateDsp(mix, sr, st, &err, o));
  // no neural separator in this environment: the call must say so rather than crash
  double r = 0;
  std::string which;
  if (!externalSeparatorAvailable(&which)) CHECK(!separateExternal("/nonexistent.wav", "/tmp/wezt_sep_none", st, r, &err));
}

}  // namespace

void testVocalFit() {  // fit a sung vocal to a melody
    using namespace wezt;
    const double sr = 22050;
    const double srcHz[4] = {196.0, 220.0, 247.0, 262.0};   // what was sung
    AudioBuffer v(1, static_cast<std::size_t>(sr * 4.0));
    for (int n = 0; n < 4; ++n) {
      const std::size_t a = static_cast<std::size_t>((n + 0.1) * sr), b = static_cast<std::size_t>((n + 0.9) * sr);
      for (std::size_t i = a; i < b; ++i) {
        const double t = static_cast<double>(i) / sr, e = std::min({1.0, (i - a) / 800.0, (b - i) / 800.0});
        v.channel(0)[i] = static_cast<float>(e * (0.4 * std::sin(2 * M_PI * srcHz[n] * t) + 0.15 * std::sin(4 * M_PI * srcHz[n] * t)));
      }
    }
    music::MidiClip mel;
    const int target[4] = {62, 64, 65, 67};   // D4 E4 F4 G4 at 120 bpm, one note per second pair
    for (int n = 0; n < 4; ++n) mel.notes.push_back({target[n], n * 1.0, 0.9, 100});   // 0.5 s each at 120 bpm
    music::VocalFitParams p; p.bpm = 120; p.key = {2, music::Mode::Major};
    const auto fit = music::fitVocal(v, sr, mel, p);
    CHECK(fit.ok);
    CHECK(fit.sourceNotes == 4 && fit.targetNotes == 4);
    CHECK(fit.lead.frames > 0 && fit.harmony.frames == fit.lead.frames);
    const auto pf = music::trackPitch(fit.lead, sr);
    for (int n = 0; n < 4; ++n) {
      std::vector<double> m;
      const double t0 = n * 0.5 + 0.1, t1 = n * 0.5 + 0.35;
      for (auto& f : pf) if (f.hz > 0 && f.confidence > 0.5 && f.time > t0 && f.time < t1) m.push_back(69 + 12 * std::log2(f.hz / 440.0));
      CHECK(!m.empty());
      if (!m.empty()) { std::sort(m.begin(), m.end()); { double d = std::fmod(m[m.size() / 2] - target[n] + 600.0, 12.0); if (d > 6) d -= 12; CHECK(std::fabs(d) < 0.6); }  // same note, nearest octave to the singer }
    }
    {  // choir: one voice a diatonic third above the first sung note G3 in G major = B3 (59)
      music::ChoirParams cp; cp.key = {7, music::Mode::Major}; cp.voices = 1; cp.spreadMs = 5;
      const AudioBuffer ch = music::makeChoir(v, sr, cp);
      CHECK(ch.channels == 2 && ch.frames >= v.frames && ch.peak() > 0.02f);
      AudioBuffer m1(1, ch.frames);
      std::copy(ch.channel(0), ch.channel(0) + ch.frames, m1.channel(0));
      for (std::size_t i = 0; i < ch.frames; ++i) m1.channel(0)[i] += ch.channel(1)[i];
      std::vector<double> m;
      for (auto& f : music::trackPitch(m1, sr)) if (f.hz > 0 && f.confidence > 0.5 && f.time > 0.2 && f.time < 0.45) m.push_back(69 + 12 * std::log2(f.hz / 440.0));
      CHECK(!m.empty());
      if (!m.empty()) { std::sort(m.begin(), m.end()); CHECK(std::fabs(m[m.size() / 2] - 59.0) < 0.7); }
      music::ChoirParams big; big.key = cp.key; big.voices = 4;
      CHECK(music::makeChoir(v, sr, big).peak() > 0.02f);
      CHECK(music::makeChoir(AudioBuffer(), sr, big).frames == 0);
    }
    CHECK(!music::fitVocal(AudioBuffer(), sr, mel, p).ok);
    AudioBuffer silent(1, 22050);
    CHECK(!music::fitVocal(silent, sr, mel, p).ok);
  }
}

int main() {
  testTheory();
  testProgression();
  testMelodyAndParts();
  testMidiFile();
  testSynth();
  testSong();
  testAudioToMidi();
  testSeparation();
  testVocalFit();
  std::printf(failures ? "FAILED\n" : "music tests passed\n");
  return failures;
}