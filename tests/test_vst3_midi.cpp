// Sends notes to a real VST3 instrument (a class from the SDK's mda-vst3 bundle) through
// Vst3Instance::handleMidi and checks the audio: silence before the note, sound after it, the right
// pitch, and a release after note-off. Usage: wezt_vst3_midi_test <bundle.vst3> <class name> | --list
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "engine/engine.h"
#include "engine/graph.h"
#include "plugins/vst3_host.h"

using namespace wezt;
using namespace wezt::plugins;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static MidiEvent ev(MidiEvent::Type t, std::uint32_t off, int a, int b = 0) {
  MidiEvent e; e.type = t; e.offset = off; e.a = static_cast<std::int16_t>(a); e.b = static_cast<std::int16_t>(b); return e;
}
static float peak(const std::vector<float>& v, std::size_t from, std::size_t to) {
  float m = 0;
  for (std::size_t i = from; i < std::min(to, v.size()); ++i) m = std::max(m, std::fabs(v[i]));
  return m;
}
// Fundamental by normalised autocorrelation between 80 and 2000 Hz. Takes the shortest lag whose
// peak is within 10% of the best one, which avoids picking a multiple of the period (octave errors).
static double fundamental(const std::vector<float>& v, std::size_t from, std::size_t to, double sr) {
  const std::size_t lo = static_cast<std::size_t>(sr / 2000.0), hi = static_cast<std::size_t>(sr / 80.0);
  std::vector<double> r(hi + 2, 0.0);
  double best = 0;
  for (std::size_t lag = lo; lag <= hi + 1; ++lag) {
    double s = 0, e1 = 0, e2 = 0;
    for (std::size_t i = from; i + lag < to; ++i) { s += v[i] * v[i + lag]; e1 += v[i] * v[i]; e2 += v[i + lag] * v[i + lag]; }
    r[lag] = s / std::sqrt(e1 * e2 + 1e-30);
    if (lag <= hi) best = std::max(best, r[lag]);
  }
  for (std::size_t lag = lo + 1; lag <= hi; ++lag)
    if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {
      // parabolic refinement of the peak position
      const double a = r[lag - 1], b = r[lag], c = r[lag + 1], den = a - 2 * b + c;
      const double shift = den != 0 ? 0.5 * (a - c) / den : 0.0;
      return sr / (static_cast<double>(lag) + shift);
    }
  return 0.0;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("usage: wezt_vst3_midi_test <bundle.vst3> <class name> | --list  (skipped)\n"); return 0; }
  std::string err;
  if (argc >= 3 && std::string(argv[2]) == "--list") {
    for (auto& c : describeVst3(argv[1], &err)) std::printf("%s | %s | %s\n", c.name.c_str(), c.category.c_str(), c.subCategories.c_str());
    return 0;
  }
  const std::string cls = argc >= 3 ? argv[2] : "";
  constexpr std::size_t kBlock = 256;
  constexpr double kSr = 48000.0;
  CHECK(!Vst3Instance::load(argv[1], &err, "no such class here"));
  CHECK(err.find("no such class here") != std::string::npos);

  auto inst = Vst3Instance::load(argv[1], &err, cls);
  if (!inst) { std::printf("load failed: %s\n", err.c_str()); return 1; }
  CHECK(inst->acceptsMidi());
  Vst3Instance* plug = inst.get();
  Graph g;
  auto n = g.add(std::move(inst));
  g.setOutput(n);
  Engine engine(g, kBlock, 2, kSr, 5);
  AudioBuffer out;
  std::vector<float> left;
  auto block = [&](std::vector<MidiEvent> events) {
    plug->handleMidi(events.data(), events.size());
    g.render(out);
    left.insert(left.end(), out.channel(0), out.channel(0) + kBlock);
  };

  // Different plugins ship different default patches (JX10 has a slow attack and a very long release, DX10
  // is FM), so release time and absolute pitch are only asserted for the plain piano. What holds for every
  // instrument is: silence before the note, sound after it, and the key number reaching the plugin (an
  // octave higher in key is twice the frequency).
  const bool strict = cls == "mda Piano";
  for (int i = 0; i < 4; ++i) block({});                         // settle
  const std::size_t base = left.size();
  block({ev(MidiEvent::NoteOn, 100, 69, 110)});
  for (int i = 0; i < 60; ++i) block({});                        // hold ~320 ms
  const std::size_t held = left.size();
  block({ev(MidiEvent::NoteOff, 10, 69)});
  for (int i = 0; i < 400; ++i) block({});                       // let the release run

  const float before = peak(left, 0, base + 100);
  const float after = peak(left, base + 100, held);
  std::printf("%s: peak before note %.6f, while held %.4f\n", cls.c_str(), before, after);
  CHECK(before < 1e-4f);                                         // silent until the note
  CHECK(after > 0.01f);                                          // a note-on really sounded
  if (strict) {
    const double f = fundamental(left, base + 12 * kBlock, held, kSr);
    std::printf("fundamental %.2f Hz (A4 = 440)\n", f);
    CHECK(std::fabs(f - 440.0) < 6.0);
    CHECK(peak(left, left.size() - 20 * kBlock, left.size()) < 0.2f * after);  // the note-off released it
  }

  // Relative pitch: key 60 and key 72 (one octave apart), each after a panic and a pause.
  auto measure = [&](int key) {
    block({ev(MidiEvent::AllNotesOff, 0, 0)});
    for (int i = 0; i < 300; ++i) block({});
    const std::size_t s0 = left.size();
    block({ev(MidiEvent::NoteOn, 0, key, 110)});
    for (int i = 0; i < 80; ++i) block({});
    return fundamental(left, s0 + 40 * kBlock, left.size(), kSr);
  };
  const double lo = measure(60), hi = measure(72);
  std::printf("key 60 -> %.2f Hz, key 72 -> %.2f Hz, ratio %.3f\n", lo, hi, hi / std::max(1.0, lo));
  CHECK(lo > 0.0 && std::fabs(hi / lo - 2.0) < 0.06);
  CHECK(!strict || std::fabs(lo - 261.63) < 4.0);

  // Panic silences a held note: the level drops well below what it was while held.
  block({ev(MidiEvent::NoteOn, 0, 60, 100)});
  for (int i = 0; i < 60; ++i) block({});
  const float heldLevel = peak(left, left.size() - 8 * kBlock, left.size());
  block({ev(MidiEvent::AllNotesOff, 0, 0)});
  for (int i = 0; i < 400; ++i) block({});
  CHECK(heldLevel > 0.01f);
  CHECK(peak(left, left.size() - 10 * kBlock, left.size()) < 0.2f * heldLevel);

  std::printf(failures ? "FAILED\n" : "vst3 midi tests passed\n");
  return failures ? 1 : 0;
}
