// Editing layer: undo/redo, memory locations, strip silence, transients, clip gain, shuffle,
// automation writing modes, VCA, and saving all of it.
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "edit/clip_tools.h"
#include "edit/memory_locations.h"
#include "edit/undo.h"
#include "session/automation_writer.h"
#include "session/session.h"

using namespace wezt;
using namespace wezt::edit;
using namespace wezt::session;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("FAIL %s:%d %s=%g vs %g\n", __FILE__, __LINE__, #a, _a, _b); ++failures; } } while (0)

namespace {
constexpr double kRate = 48000.0;

// 1 s of silence with two bursts: 0.2-0.3 s and 0.7-0.8 s, amplitude 0.5.
Clip burstClip() {
  auto b = std::make_shared<AudioBuffer>(1, 48000);
  for (int i = 9600; i < 14400; ++i) b->channel(0)[i] = 0.5f * std::sin(i * 0.3f);
  for (int i = 33600; i < 38400; ++i) b->channel(0)[i] = 0.5f * std::sin(i * 0.3f);
  Clip c;
  c.id = 1; c.audio = b; c.start = 1000; c.offset = 0; c.length = 48000;
  return c;
}

void testUndo() {
  UndoStack u(3);
  int v = 0;
  auto step = [&](int to, const char* label, const char* key = "") {
    const int from = v;
    u.perform({label, [&v, from] { v = from; }, [&v, to] { v = to; }, key});
  };
  step(1, "a"); step(2, "b");
  CHECK(v == 2 && u.undoLabel() == "b");
  CHECK(u.undo() && v == 1);
  CHECK(u.redo() && v == 2);
  CHECK(!u.redo());
  u.undo();
  step(5, "c");                      // a new edit clears redo
  CHECK(!u.canRedo() && v == 5);
  step(6, "drag", "fader"); step(7, "drag", "fader"); step(8, "drag", "fader");
  CHECK(u.history().size() <= 3);    // depth limit, and drags merged into one
  u.undo();
  CHECK(v == 5);                     // one undo undoes the whole drag
  u.undo(); u.undo();
  CHECK(!u.canUndo());
}

void testMemory() {
  MemoryLocations m;
  CHECK(m.add({0, "verse", "", 48000, 48000}) == 1);
  CHECK(m.add({0, "chorus", "", 96000, 192000}) == 2);
  CHECK(m.add({0, "x", "", 10, 10}, 2) == 3);  // 2 is taken: next free
  CHECK(m.add({0, "bridge", "", 5, 5}, 9) == 9);
  CHECK(m.find(2)->isRange());
  CHECK(m.next(48000)->number == 2);
  CHECK(m.previous(48000)->number == 3);
  CHECK(m.next(1000000) == nullptr && m.previous(0) == nullptr);
  CHECK(m.sorted().front().number == 9);
  CHECK(m.remove(1) && !m.remove(1));
}

void testStripSilence() {
  Clip c = burstClip();
  StripSilenceParams p;
  const auto parts = stripSilence(c, kRate, p);
  CHECK(parts.size() == 2);
  if (parts.size() == 2) {
    CHECK(parts[0].start >= c.start + 9000 && parts[0].start <= c.start + 9600);
    CHECK(parts[0].start + parts[0].length >= c.start + 14400);
    CHECK(parts[1].offset >= 33000 && parts[1].offset <= 33600);
    CHECK(parts[0].fadeIn > 0 && parts[0].fadeOut > 0);
  }
  // A long minimum silence keeps everything as one piece.
  p.minSilenceMs = 1000;
  CHECK(stripSilence(c, kRate, p).size() == 1);
  // Louder threshold than the signal: nothing left.
  p.minSilenceMs = 100; p.thresholdDb = 0.f;
  CHECK(stripSilence(c, kRate, p).empty());
  // Silent clip.
  Clip s = c; s.audio = std::make_shared<AudioBuffer>(1, 48000);
  CHECK(stripSilence(s, kRate, {}).empty());
}

void testTransientsAndGain() {
  Clip c = burstClip();
  const auto hits = transientPositions({c}, kRate, 0.5f);
  CHECK(hits.size() >= 2);
  CHECK(tabToNextTransient(hits, 0) == hits.front());
  CHECK(tabToNextTransient(hits, hits.back()) == -1);
  CHECK(tabToPreviousTransient(hits, hits.back()) == hits[hits.size() - 2]);
  CHECK(tabToPreviousTransient(hits, 0) == -1);
  NEAR(clipPeak(c), 0.5, 0.01);
  NEAR(normalizeGainDb(c, -6.f), -6.0 - 20 * std::log10(clipPeak(c)), 1e-3);
  Clip silent = c; silent.audio = std::make_shared<AudioBuffer>(1, 100);
  CHECK(normalizeGainDb(silent) == 0.f);
}

void testSeparateShuffleSnap() {
  Clip c = burstClip();
  c.fadeIn = 100; c.fadeOut = 200;
  c.gainEnv.add(0, 0.f); c.gainEnv.add(48000, -12.f);
  Clip l, r;
  CHECK(!separateAt(c, c.start, l, r) && !separateAt(c, c.start + c.length, l, r));
  CHECK(separateAt(c, c.start + 24000, l, r));
  CHECK(l.length == 24000 && r.length == 24000 && r.start == c.start + 24000 && r.offset == 24000);
  CHECK(l.fadeIn == 100 && l.fadeOut == 0 && r.fadeIn == 0 && r.fadeOut == 200);
  NEAR(r.gainEnv.valueAt(0, 0.f), -6.0, 1e-3);  // the envelope continues across the cut
  c.locked = true;
  CHECK(!separateAt(c, c.start + 100, l, r));

  Clip a = burstClip(), b = burstClip();
  a.id = 1; a.start = 0; a.length = 1000;
  b.id = 2; b.start = 5000; b.length = 2000;
  auto sh = shuffleClose({b, a});
  CHECK(sh[0].id == 1 && sh[0].start == 0 && sh[1].start == 1000);
  auto mv = moveClip({a, b}, 2, -9000, false);
  CHECK(mv[1].start == 0);  // clamped at zero
  a.locked = true;
  CHECK(moveClip({a, b}, 1, 50, false)[0].start == 0);
  CHECK(snapToGrid(1100, 1000) == 1000 && snapToGrid(1500, 1000) == 2000 && snapToGrid(77, 0) == 77);
}

void testRenderClip() {
  Clip c = burstClip();
  c.gainDb = -6.f;
  c.gainEnv.add(0, 0.f); c.gainEnv.add(48000, 0.f);
  auto r = renderClip(c);
  CHECK(r && r->frames == 48000);
  NEAR(r->peak(), clipPeak(c) * std::pow(10.0, -6.0 / 20), 0.01);
}

void testAutomationWriter() {
  auto fill = [] { AutomationLane l; l.add(0, 0.f); l.add(10000, 0.f); return l; };
  // Touch: writes only while held, then rejoins the old line.
  {
    AutomationLane lane = fill();
    AutomationWriter w; w.setMode(AutomationMode::Touch);
    CHECK(!w.update(100, -3.f, false));
    CHECK(w.update(1000, -3.f, true));
    CHECK(w.update(2000, -6.f, true));
    CHECK(!w.update(3000, -6.f, false));
    CHECK(w.hasPending());
    NEAR(lane.valueAt(1500, 0), 0, 1e-6);  // nothing written during playback
    CHECK(w.commit(lane, 500) >= 3);
    NEAR(lane.valueAt(1500, 99), -4.5, 1e-3);
    NEAR(lane.valueAt(2000, 99), -6, 1e-3);
    NEAR(lane.valueAt(2500, 99), -6, 1e-3);  // still holding until released at 3000
    NEAR(lane.valueAt(3500, 99), 0, 1e-3);   // back on the old automation
    NEAR(lane.valueAt(8000, 99), 0, 1e-3);
  }
  // Latch: keeps the last value until the transport stops.
  {
    AutomationLane lane = fill();
    AutomationWriter w; w.setMode(AutomationMode::Latch);
    CHECK(!w.update(100, 1.f, false));
    CHECK(w.update(1000, -2.f, true));
    CHECK(w.update(2000, -2.f, false));    // released, still writing
    CHECK(w.update(4000, -2.f, false));
    w.endPass(4000);
    w.commit(lane);
    NEAR(lane.valueAt(3000, 99), -2, 1e-3);
  }
  // Touch/latch: a fader acts like Touch, other controls like Latch.
  {
    AutomationWriter w; w.setMode(AutomationMode::TouchLatch); w.setIsFader(true);
    w.update(10, 1.f, true);
    CHECK(!w.update(20, 1.f, false));
    AutomationWriter w2; w2.setMode(AutomationMode::TouchLatch); w2.setIsFader(false);
    w2.update(10, 1.f, true);
    CHECK(w2.update(20, 1.f, false));
  }
  // Write: from the start, no touching needed. Read / Off never write.
  {
    AutomationLane lane = fill();
    AutomationWriter w; w.setMode(AutomationMode::Write);
    CHECK(w.update(0, 5.f, false));
    w.update(5000, 5.f, false);
    w.endPass(5000);
    w.commit(lane);
    NEAR(lane.valueAt(2500, 0), 5, 1e-3);
    AutomationWriter r; r.setMode(AutomationMode::Read);
    CHECK(!r.update(0, 1.f, true) && !r.hasPending());
  }
  // Thin: a straight ramp with extra points collapses to its end points; a corner survives.
  {
    AutomationLane l;
    for (int i = 0; i <= 100; ++i) l.add(i * 10, i * 0.1f);
    l.thin(0.01f);
    CHECK(l.points().size() == 2);
    AutomationLane k;
    for (int i = 0; i <= 50; ++i) k.add(i * 10, i * 0.1f);
    for (int i = 51; i <= 100; ++i) k.add(i * 10, 5.f - (i - 50) * 0.1f);
    k.thin(0.01f);
    CHECK(k.points().size() == 3);
  }
}

void testSessionVcaAndSave() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "wezt_edit_test";
  fs::remove_all(dir); fs::create_directories(dir);
  Session s(kRate, 2, 256);
  const TrackId a = s.addTrack("A"), b = s.addTrack("B"), c = s.addTrack("C");
  const VcaId v = s.addVca("Drums", {a, b, 777});
  CHECK(s.vca(v)->members.size() == 2);
  CHECK(s.setVcaGain(v, -6.f));
  NEAR(s.track(a)->strip->vcaDb(), -6, 1e-6);
  NEAR(s.track(c)->strip->vcaDb(), 0, 1e-6);
  const VcaId v2 = s.addVca("All", {a, c});
  s.setVcaGain(v2, -2.f);
  NEAR(s.track(a)->strip->vcaDb(), -8, 1e-6);   // VCAs add up
  s.removeTrack(b);
  CHECK(s.vca(v)->members.size() == 1);

  s.memory().add({0, "verse 2", "check the snare", 48000, 96000, 2, {0, 2}, 100, 200});
  Clip clip = burstClip();
  clip.name = "snare take";
  clip.muted = true;
  clip.gainEnv.add(0, -3.f); clip.gainEnv.add(1000, 2.f);
  clip.audio = nullptr;  // no file on disk for this test: it loads back as a warning
  clip.file = "missing.wav";
  s.addClip(a, clip);
  CHECK(s.save((dir / "t.wezt").string()));
  std::string err;
  auto l = Session::load((dir / "t.wezt").string(), {}, &err);
  CHECK(l != nullptr);
  if (l) {
    CHECK(l->vcaIds().size() == 2);
    CHECK(l->memory().all().size() == 1);
    const auto* m = l->memory().find(1);
    CHECK(m && m->name == "verse 2" && m->comment == "check the snare" && m->start == 48000 && m->end == 96000 && m->zoomPreset == 2 && m->preRoll == 100 && m->visibleTracks.size() == 2);
    const auto ids = l->trackIds();
    CHECK(ids.size() == 2);
    NEAR(l->track(ids[0])->strip->vcaDb(), -8, 1e-6);   // members survive the round trip, with b removed before saving
    NEAR(l->track(ids[1])->strip->vcaDb(), -2, 1e-6);
    const auto pl = l->track(ids[0])->source->playlist();
    CHECK(pl.size() == 1 && pl[0].name == "snare take" && pl[0].muted && pl[0].gainEnv.points().size() == 2);
  }
  fs::remove_all(dir);
}

void testMutedClipAndEnvelopePlayback() {
  Session s(kRate, 2, 256);
  const TrackId t = s.addTrack("T", true);
  auto buf = std::make_shared<AudioBuffer>(1, 4096);
  for (std::size_t i = 0; i < 4096; ++i) buf->channel(0)[i] = 1.f;
  Clip c; c.audio = buf; c.start = 0; c.length = 4096;
  c.gainEnv.add(0, -6.f); c.gainEnv.add(4096, -6.f);
  s.addClip(t, c);
  Engine e(s.graph(), 256, 2, kRate, 2);
  s.attach(e.context());
  std::vector<float> l(256), r(256);
  float* out[2] = {l.data(), r.data()};
  e.transport().locate(0); e.transport().play();
  e.process(nullptr, out, 256);
  e.process(nullptr, out, 256);
  const float expected = std::pow(10.f, -6.f / 20.f) * std::cos(0.78539816f);  // clip gain, then mono pan law
  NEAR(l[100], expected, 0.01);
  // Mute the clip: silence.
  auto pl = s.track(t)->source->playlist();
  pl[0].muted = true;
  s.track(t)->source->setPlaylist(pl);
  e.transport().locate(0);
  e.process(nullptr, out, 256);
  e.process(nullptr, out, 256);
  NEAR(l[100], 0.0, 1e-6);
}

}  // namespace

int main() {
  testUndo();
  testMemory();
  testStripSilence();
  testTransientsAndGain();
  testSeparateShuffleSnap();
  testRenderClip();
  testAutomationWriter();
  testSessionVcaAndSave();
  testMutedClipAndEnvelopePlayback();
  std::printf(failures ? "FAILED\n" : "edit tests passed\n");
  return failures;
}
