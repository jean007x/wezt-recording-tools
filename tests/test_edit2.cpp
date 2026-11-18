#include <cmath>
#include <cstdio>
#include <memory>

#include "edit/beat_detective.h"
#include "edit/clip_tools.h"
#include "edit/piano_roll.h"
#include <filesystem>

#include "io/wav.h"
#include "music/midi_edit.h"
#include "music/notation.h"
#include "session/session.h"

using namespace wezt;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void testBeatDetective() {
  const double sr = 44100;
  const double jitter[8] = {0.000, 0.021, -0.015, 0.030, -0.025, 0.012, 0.018, -0.010};   // seconds off the grid
  auto buf = std::make_shared<AudioBuffer>(1, static_cast<std::size_t>(sr * 4.5));
  unsigned s = 12345;
  for (int n = 0; n < 8; ++n) {
    const std::int64_t at = static_cast<std::int64_t>((0.5 * n + 0.25 + jitter[n]) * sr);   // 120 bpm, quarter grid offset by 0.25 s -> use eighth grid 0.25 s
    for (int i = 0; i < 2000; ++i) {
      s = s * 1664525u + 1013904223u;
      const float noise = (static_cast<float>(s >> 8) / 8388608.f) - 1.f;
      buf->channel(0)[static_cast<std::size_t>(at + i)] = 0.8f * noise * std::exp(-static_cast<float>(i) / 300.f);
    }
  }
  session::Clip c; c.id = 1; c.audio = buf; c.start = 0; c.length = static_cast<std::int64_t>(buf->frames);
  edit::BeatDetectiveParams p; p.bpm = 120; p.gridBeats = 0.5; p.strength = 1.f;   // eighth notes = 0.25 s
  const auto r = edit::beatDetective({c}, sr, p);
  CHECK(r.hits >= 6);
  CHECK(r.clips.size() >= 7);
  const double grid = 0.25 * sr;
  int onGrid = 0, tested = 0;
  for (const auto& cl : r.clips) {
    const double hit = static_cast<double>(cl.start) + 0.003 * sr;
    const double d = std::fabs(hit / grid - std::round(hit / grid)) * grid / sr * 1000.0;   // ms from the grid
    ++tested; if (d < 4.0) ++onGrid;
  }
  CHECK(tested >= 7 && onGrid >= tested - 1);
  // strength 0 changes nothing
  p.strength = 0.f;
  const auto z = edit::beatDetective({c}, sr, p);
  CHECK(z.moved == 0);
  // clips must not overlap beyond the fade
  auto cl = r.clips;
  for (std::size_t i = 0; i + 1 < cl.size(); ++i) CHECK(cl[i].start + cl[i].length <= cl[i + 1].start + static_cast<std::int64_t>(0.006 * sr));
  // a locked clip is left alone
  session::Clip lk = c; lk.locked = true;
  const auto l = edit::beatDetective({lk}, sr, edit::BeatDetectiveParams{});
  CHECK(l.clips.size() == 1 && l.clips[0].start == 0);
}

static void testMidiEdit() {
  music::MidiClip c;
  c.notes = {{60, 0.07, 0.4, 80}, {64, 1.03, 0.5, 90}, {67, 1.98, 0.3, 100}, {72, 3.04, 0.2, 110}};
  auto q = music::quantizeMidi(c, 1.0);
  CHECK(std::fabs(q.notes[0].start - 0.0) < 1e-9 && std::fabs(q.notes[2].start - 2.0) < 1e-9);
  auto half = music::quantizeMidi(c, 1.0, 0.5);
  CHECK(std::fabs(half.notes[0].start - 0.035) < 1e-9);
  auto t = music::transposeMidi(c, 12);
  CHECK(t.notes[0].pitch == 72 && t.notes[3].pitch == 84);
  music::Key cmaj{0, music::Mode::Major};
  auto tk = music::transposeMidi(c, 1, true, cmaj);   // 61 63... must land in C major
  for (auto& n : tk.notes) { const int pc = n.pitch % 12; CHECK(pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11); }
  auto h = music::humanizeMidi(c, 0.05, 10, 3), h2 = music::humanizeMidi(c, 0.05, 10, 3);
  CHECK(h.notes[1].start == h2.notes[1].start);                       // same seed, same result
  for (auto& n : h.notes) CHECK(n.velocity >= 1 && n.velocity <= 127 && n.start >= 0);
  auto lg = music::legatoMidi(c);
  CHECK(std::fabs(lg.notes[0].length - (1.03 - 0.07)) < 1e-9);
  auto v = music::scaleVelocity(c, 2.0, 0);
  CHECK(v.notes[3].velocity == 127);
  auto cr = music::cropMidi(c, 1.0, 3.0);
  CHECK(cr.notes.size() == 2 && std::fabs(cr.notes[0].start - 0.03) < 1e-9);
  CHECK(music::pitchName(60) == "C4" && music::pitchName(69) == "A4");
  CHECK(music::describeMidi(c).find("Bar 1:") != std::string::npos);
  CHECK(music::quantizeMidi(c, 0.0).notes.size() == 4);   // bad grid: unchanged
  // round trip through a file
  const auto bytes = music::writeMidiFile({c}, 100);
  music::MidiFile mf; CHECK(music::readMidiFile(bytes, mf) && mf.clips.size() == 1 && mf.clips[0].notes.size() == 4);
}


static std::string makeWav(const std::string& dir, const char* name, float level, double seconds, double sr) {
  AudioBuffer b(1, static_cast<std::size_t>(seconds * sr));
  for (std::size_t i = 0; i < b.frames; ++i) b.channel(0)[i] = level * std::sin(static_cast<float>(i) * 0.05f);
  const std::string p = dir + "/" + name;
  io::writeWavFloat(p, b, static_cast<unsigned>(sr), name);
  return p;
}

static void testPlaylists() {
  namespace fs = std::filesystem;
  const std::string dir = (fs::temp_directory_path() / "wezt_playlist_test").string();
  fs::remove_all(dir); fs::create_directories(dir);
  const double sr = 22050;
  session::Session s(sr, 2, 256);
  const auto t = s.addTrack("Vox", true, {});
  std::string e;
  const int a = s.addClipFromFile(t, makeWav(dir, "take1.wav", 0.5f, 4.0, sr), 0, &e);
  CHECK(a > 0);
  CHECK(s.livePlaylist(t) == "Main" && s.playlistNames(t).size() == 1);
  CHECK(s.newPlaylist(t, "Take 2", false));                 // empty, live
  CHECK(s.track(t)->source->playlist().empty());
  CHECK(s.addClipFromFile(t, makeWav(dir, "take2.wav", 0.25f, 4.0, sr), 0, &e) > 0);
  CHECK(!s.newPlaylist(t, "Take 2", true));                 // names are unique
  CHECK(s.playlistNames(t).size() == 2 && s.livePlaylist(t) == "Take 2");
  CHECK(s.switchPlaylist(t, "Main"));
  CHECK(s.track(t)->source->playlist().size() == 1 && std::fabs(s.track(t)->source->playlist()[0].audio->channel(0)[20]) > 0.1f);
  CHECK(!s.switchPlaylist(t, "nope"));
  // comp: 1 s..2 s of Take 2 into Main
  CHECK(s.compFromPlaylist(t, "Take 2", static_cast<std::int64_t>(sr), static_cast<std::int64_t>(2 * sr)));
  const auto pl = s.track(t)->source->playlist();
  CHECK(pl.size() == 3);
  if (pl.size() == 3) {
    CHECK(pl[0].start == 0 && pl[0].length == static_cast<std::int64_t>(sr));
    CHECK(pl[1].start == static_cast<std::int64_t>(sr) && pl[1].length == static_cast<std::int64_t>(sr));
    CHECK(pl[2].start == static_cast<std::int64_t>(2 * sr) && pl[2].start + pl[2].length == static_cast<std::int64_t>(4 * sr));
    // the middle piece plays Take 2's audio (lower level), its neighbours Take 1's
    const float mid = std::fabs(pl[1].audio->channel(0)[static_cast<std::size_t>(pl[1].offset) + 20]);
    const float outer = std::fabs(pl[0].audio->channel(0)[20]);
    CHECK(mid < outer);
  }
  CHECK(s.renamePlaylist(t, "Take 2", "Best chorus") && !s.renamePlaylist(t, "Main", "Best chorus"));
  // save and load keep every playlist
  CHECK(s.save(dir + "/s.wezt", &e));
  std::string err;
  auto l = session::Session::load(dir + "/s.wezt", {}, &err);
  CHECK(l != nullptr);
  if (l) {
    const auto ids = l->trackIds();
    CHECK(ids.size() == 1);
    const auto names = l->playlistNames(ids[0]);
    CHECK(names.size() == 2 && names[0] == "Main" && names[1] == "Best chorus");
    CHECK(l->track(ids[0])->source->playlist().size() == 3);
    CHECK(l->switchPlaylist(ids[0], "Best chorus") && l->track(ids[0])->source->playlist().size() == 1);
    CHECK(l->track(ids[0])->source->playlist()[0].audio != nullptr);
  }
  CHECK(s.deletePlaylist(t, "Best chorus") && !s.deletePlaylist(t, "Main"));
  // duplicate keeps the alternates
  s.newPlaylist(t, "X", true);
  const auto d = s.duplicateTrack(t);
  CHECK(d >= 0 && s.playlistNames(d).size() == s.playlistNames(t).size());
}


static void testPianoRoll() {
  edit::PianoRoll r;
  CHECK(r.addNote(60, 0.0, 1.0, 100, 0.25) == 0);
  CHECK(r.addNote(64, 1.1, 1.0, 90, 0.25) >= 0);              // snaps to 1.0
  CHECK(std::fabs(r.note(1).start - 1.0) < 1e-9);
  CHECK(r.addNote(64, 1.0, 1.0, 90, 0.25) == -1);             // same pitch and start: refused
  CHECK(r.addNote(67, 2.0, 1.0, 80, 0.25) >= 0 && r.size() == 3);
  std::size_t idx = 99;
  CHECK(r.hitTest(0.5, 60, 0.1, idx) == edit::PianoRoll::Hit::Body && idx == 0);
  CHECK(r.hitTest(0.95, 60, 0.1, idx) == edit::PianoRoll::Hit::ResizeEnd);
  CHECK(r.hitTest(0.5, 61, 0.1, idx) == edit::PianoRoll::Hit::None);
  r.selectNone(); CHECK(r.selectedCount() == 0);
  r.selectRect(0.9, 2.1, 60, 64, false);                       // notes 60 (ends 1.0) and 64
  CHECK(r.selectedCount() == 2);
  r.moveSelected(0.5, 2, 0.25);
  CHECK(std::fabs(r.note(0).start - 0.5) < 1e-9 && r.note(0).pitch == 62 && r.note(1).pitch == 66);
  r.moveSelected(-10, 0, 0);                                   // cannot go before 0
  CHECK(r.note(0).start >= 0);
  r.moveSelected(0, 200, 0);                                   // clamped to 127
  int top = 0; for (std::size_t i = 0; i < r.size(); ++i) if (r.selected(i)) top = std::max(top, r.note(i).pitch);
  CHECK(top == 127);
  r.moveSelected(0, -200, 0);
  r.selectAll(); r.transposeSelected(2); r.transposeSelected(-2);
  r.setVelocity(64);
  for (std::size_t i = 0; i < r.size(); ++i) CHECK(r.note(i).velocity == 64);
  r.scaleVelocity(100.0);
  CHECK(r.note(0).velocity == 127);
  r.selectNone(); r.select(0, false); r.resizeSelected(0.5, 0.25);
  CHECK(r.note(0).length > 1.4);
  r.resizeSelected(-100, 0.25);
  CHECK(std::fabs(r.note(0).length - 0.25) < 1e-9);            // never shorter than the grid
  r.selectAll(); r.copySelected();
  const std::size_t n = r.size();
  r.paste(10.0);
  CHECK(r.size() == 2 * n && r.selectedCount() == n);
  r.duplicateSelected();
  CHECK(r.size() == 3 * n);
  r.deleteSelected();                                          // the duplicates were selected
  CHECK(r.size() == 2 * n);
  r.selectAll(); r.quantizeSelected(1.0);
  for (std::size_t i = 0; i < r.size(); ++i) CHECK(std::fabs(r.note(i).start - std::round(r.note(i).start)) < 1e-9);
  const auto c = r.clip();
  for (std::size_t i = 1; i < c.notes.size(); ++i) CHECK(c.notes[i - 1].start <= c.notes[i].start);   // stays sorted
  edit::PianoRoll r2(c); CHECK(r2.size() == r.size());
}


static void testNotation() {
  using namespace music;
  CHECK(keyFifths({0, Mode::Major}) == 0 && keyFifths({7, Mode::Major}) == 1 && keyFifths({9, Mode::Minor}) == 0);
  CHECK(keyFifths({5, Mode::Major}) == -1 && keyFifths({2, Mode::Minor}) == -1 && keyFifths({11, Mode::Major}) == 5);
  CHECK(spellPitch(60, 0) == "C4" && spellPitch(61, 0) == "C#4" && spellPitch(70, -1) == "Bb4" && spellPitch(70, 2) == "A#4");
  CHECK(spellPitch(66, 2) == "F#4" && spellPitch(59, 0) == "B3" && spellPitch(48, 0) == "C3");
  CHECK(stepOf(60, 0) == 28 && stepOf(64, 0) == 30 && bottomLineStep(Clef::Treble) == 30 && bottomLineStep(Clef::Bass) == 18);
  MidiClip c;
  c.notes = {{60, 0, 1, 90}, {62, 1, 1, 90}, {64, 2, 2, 90}};      // C D E(half)
  auto s = engrave(c, {0, Mode::Major});
  CHECK(s.bars.size() == 1 && s.clef == Clef::Treble && s.bars[0].events.size() == 3);
  CHECK(s.bars[0].events[2].value == 1 && s.bars[0].events[2].dots == 0 && s.bars[0].events[2].notes[0].name == "E4");
  // a note of 5 beats: a whole note tied to a quarter in the next bar
  MidiClip l; l.notes = {{67, 0, 5, 90}};
  s = engrave(l, {0, Mode::Major});
  CHECK(s.bars.size() == 2 && s.bars[0].events.size() == 1 && s.bars[0].events[0].value == 0 && s.bars[0].events[0].tieToNext);
  CHECK(s.bars[1].events[0].value == 2 && !s.bars[1].events[0].rest && !s.bars[1].events[0].tieToNext && s.bars[1].events.size() == 3 && s.bars[1].events[1].rest && s.bars[1].events[2].rest);
  // three beats = dotted half; gaps become rests; a half note starting off the half-bar is split by a tie
  MidiClip d; d.notes = {{60, 1, 3, 90}};                          // beat 2 for 3 beats: quarter rest first, then 3 beats from beat 1 -> half + quarter tie
  s = engrave(d, {0, Mode::Major});
  CHECK(s.bars[0].events[0].rest && s.bars[0].events[0].value == 2);
  CHECK(s.bars[0].events.size() == 3 && s.bars[0].events[1].tieToNext);   // quarter (beat 1) tied to half (beat 2)
  // every bar is filled exactly
  MidiClip m; m.notes = {{60, 0.5, 0.5, 90}, {65, 1.25, 0.75, 90}, {72, 3.5, 3.0, 90}, {55, 6.0, 1.0, 90}};
  s = engrave(m, {0, Mode::Major});
  for (auto& b : s.bars) { double sum = 0; for (auto& e : b.events) sum += e.length; CHECK(std::fabs(sum - 4.0) < 1e-9); }
  // accidentals: C# in C major is printed once per bar; F# in G major is not printed (it is in the signature)
  MidiClip a; a.notes = {{61, 0, 1, 90}, {61, 1, 1, 90}, {63, 2, 1, 90}};
  s = engrave(a, {0, Mode::Major});
  CHECK(s.bars[0].events[0].notes[0].accidental == 1 && s.bars[0].events[1].notes[0].accidental == -2 && s.bars[0].events[2].notes[0].accidental == 1);
  MidiClip g; g.notes = {{66, 0, 1, 90}, {65, 1, 1, 90}};          // F# then F natural in G major
  s = engrave(g, {7, Mode::Major});
  CHECK(s.bars[0].events[0].notes[0].accidental == -2 && s.bars[0].events[1].notes[0].accidental == 0);
  // chords, bass clef, empty clip
  MidiClip ch; ch.notes = {{48, 0, 2, 90}, {52, 0, 2, 90}, {55, 0, 2, 90}};
  s = engrave(ch, {0, Mode::Major});
  CHECK(s.clef == Clef::Bass && s.bars[0].events[0].notes.size() == 3);
  s = engrave(MidiClip{}, {0, Mode::Major});
  CHECK(s.bars.size() == 1 && s.bars[0].events.size() == 1 && s.bars[0].events[0].rest && s.bars[0].events[0].value == 0);
  // overlapping notes are shown cut at the next start
  MidiClip o; o.notes = {{60, 0, 3, 90}, {62, 1, 1, 90}};
  s = engrave(o, {0, Mode::Major});
  CHECK(s.bars[0].events[0].length == 1.0);
}

int main() {
  testBeatDetective();
  testMidiEdit();
  testPlaylists();
  testPianoRoll();
  testNotation();
  std::printf(failures ? "FAILED\n" : "edit2 tests passed\n");
  return failures;
}
