// Sends notes to a real CLAP instrument binary (tests/clap_synth) through ClapInstance::handleMidi and
// checks the audio it makes: the note starts at the exact sample asked for, has the right pitch, and
// stops on note-off. Both note dialects are exercised: CLAP note events and raw MIDI bytes.
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "engine/engine.h"
#include "engine/graph.h"
#include "plugins/clap_host.h"

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
// Frequency from rising zero crossings.
static double freqOf(const std::vector<float>& v, std::size_t from, std::size_t to, double sr) {
  int crossings = 0; std::size_t first = 0, last = 0;
  for (std::size_t i = from + 1; i < std::min(to, v.size()); ++i)
    if (v[i - 1] < 0.f && v[i] >= 0.f) { if (!crossings) first = i; last = i; ++crossings; }
  return crossings < 2 ? 0.0 : (crossings - 1) * sr / static_cast<double>(last - first);
}

static void runDialect(const std::string& path, const std::string& id, bool expectNoteEvents) {
  constexpr std::size_t kBlock = 256;
  constexpr double kSr = 48000.0;
  std::string err;
  auto inst = ClapInstance::load(path, &err, id);
  CHECK(inst != nullptr);
  if (!inst) { std::printf("load failed: %s\n", err.c_str()); return; }
  CHECK(inst->acceptsMidi());
  ClapInstance* plug = inst.get();
  Graph g;
  auto n = g.add(std::move(inst));
  g.setOutput(n);
  Engine engine(g, kBlock, 2, kSr, 5);  // prepares the graph, which activates the plugin
  AudioBuffer out;
  std::vector<float> left;
  auto block = [&](std::vector<MidiEvent> events) {
    plug->handleMidi(events.data(), events.size());  // audio thread, right before process(), as the sequencer does
    g.render(out);
    left.insert(left.end(), out.channel(0), out.channel(0) + kBlock);
  };

  // Block 0: silence. Block 1: A4 (69) note-on at offset 100. Block 2-3: held. Block 4: note-off at 50.
  block({});
  block({ev(MidiEvent::NoteOn, 100, 69, 100)});
  block({}); block({});
  block({ev(MidiEvent::NoteOff, 50, 69)});
  block({}); block({});

  CHECK(peak(left, 0, kBlock + 99) < 1e-6f);                          // nothing before the note
  CHECK(peak(left, kBlock + 100, kBlock + 100 + 64) > 0.05f);         // sound right at the offset
  CHECK(peak(left, kBlock + 98, kBlock + 100) < 1e-6f);               // ...and not 2 samples earlier
  const double f = freqOf(left, 2 * kBlock, 4 * kBlock, kSr);
  std::printf("%s: measured %.2f Hz (expect 440)\n", id.c_str(), f);
  CHECK(std::fabs(f - 440.0) < 2.0);
  CHECK(peak(left, 4 * kBlock + 60, left.size()) < 1e-6f);            // stopped by the note-off
  CHECK(peak(left, 4 * kBlock, 4 * kBlock + 49) > 0.05f);             // still sounding before it
  (void)expectNoteEvents;

  // Panic: AllNotesOff silences a held note.
  block({ev(MidiEvent::NoteOn, 0, 60, 90)});
  block({});
  block({ev(MidiEvent::AllNotesOff, 0, 0)});
  const std::size_t at = left.size() - kBlock;
  CHECK(peak(left, at + 4, left.size()) < 1e-6f);
}

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("usage: wezt_clap_midi_test <synth.clap>  (skipped)\n"); return 0; }
  runDialect(argv[1], "com.wezt.test.synth", true);         // CLAP note events
  runDialect(argv[1], "com.wezt.test.synth.midi", false);   // raw MIDI bytes only
  // A plugin with no note input says so, and handleMidi on it is harmless.
  if (argc > 2) {
    std::string err;
    auto fx = ClapInstance::load(argv[2], &err);
    CHECK(fx && !fx->acceptsMidi());
    if (fx) { MidiEvent e = ev(MidiEvent::NoteOn, 0, 60, 100); fx->handleMidi(&e, 1); }
  }
  std::printf(failures ? "FAILED\n" : "clap midi tests passed\n");
  return failures ? 1 : 0;
}
