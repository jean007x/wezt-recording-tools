// Keyboard map: Pro Tools default key combinations, rebinding, text round-trip.
#include <cstdio>
#include "ui/commands.h"
using namespace wezt::ui;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool is(const Keymap& m, Key k, std::uint8_t mods, Command c) { auto r = m.lookup({k, mods}); return r && *r == c; }

int main() {
  const Keymap m = Keymap::protoolsDefaults();
  // Spot checks against the published table.
  CHECK(is(m, Key::Space, 0, Command::PlayStop));
  CHECK(is(m, Key::Space, kCtrl, Command::Record));
  CHECK(is(m, Key::F12, 0, Command::Record));
  CHECK(is(m, Key::Kp3, 0, Command::Record));
  CHECK(is(m, Key::L, kCtrl | kShift, Command::LoopPlayback));
  CHECK(is(m, Key::F1, 0, Command::ModeShuffle) && is(m, Key::F4, 0, Command::ModeGrid));
  CHECK(is(m, Key::F5, 0, Command::ToolZoomer) && is(m, Key::F6, 0, Command::ToolTrimmer) && is(m, Key::F7, 0, Command::ToolSelector));
  CHECK(is(m, Key::F8, 0, Command::ToolGrabber) && is(m, Key::F10, 0, Command::ToolPencil));
  CHECK(is(m, Key::D7, kCtrl, Command::ToolSmart));
  CHECK(is(m, Key::E, kCtrl, Command::SeparateClip) && is(m, Key::D, kCtrl, Command::Duplicate));
  CHECK(is(m, Key::Z, kCtrl | kShift, Command::Redo));
  CHECK(is(m, Key::Equals, kCtrl, Command::ToggleMixEdit));
  CHECK(is(m, Key::N, kCtrl | kShift, Command::NewTrack));
  CHECK(is(m, Key::D, kAlt | kShift, Command::DuplicateTrack));
  CHECK(is(m, Key::S, kCtrl | kStart, Command::SaveSessionAs));
  CHECK(is(m, Key::B, kCtrl | kAlt, Command::BounceMix));
  CHECK(!m.lookup({Key::Space, kCtrl | kShift}));
  CHECK(!m.lookup({Key::None, 0}));

  // Every command (except the ones that only have buttons) can be reached from the keyboard.
  for (const auto& i : allCommands()) {
    if (i.id == Command::Pause || i.id == Command::StopRecord || i.id == Command::BounceTrack) continue;
    if (m.chordsFor(i.id).empty()) { std::printf("no key for %s\n", i.name); ++failures; }
  }
  // Commands have unique names (needed for the text format).
  for (std::size_t a = 0; a < allCommands().size(); ++a)
    for (std::size_t b = a + 1; b < allCommands().size(); ++b)
      CHECK(std::string(allCommands()[a].name) != allCommands()[b].name);

  // Text round trip.
  Chord c;
  CHECK(Keymap::parse("Ctrl+Shift+N", c) && c.key == Key::N && c.mods == (kCtrl | kShift));
  CHECK(Keymap::parse("cmd+option+b", c) && c.mods == (kCtrl | kAlt));
  CHECK(!Keymap::parse("Hyper+N", c) && !Keymap::parse("", c) && !Keymap::parse("Ctrl+", c));
  CHECK(Keymap::describe({Key::N, kCtrl | kShift}) == "Ctrl+Shift+N");
  CHECK(Keymap::describe({Key::N, kCtrl | kShift}, true) == "Cmd+Shift+N");
  CHECK(Keymap::describe({Key::Equals, kCtrl}) == "Ctrl+=");
  Keymap copy;
  CHECK(copy.deserialize(m.serialize()) && copy.size() == m.size());
  for (const auto& i : allCommands())
    for (const Chord& ch : m.chordsFor(i.id)) { auto r = copy.lookup(ch); CHECK(r && *r == i.id); }
  CHECK(!copy.deserialize("Ctrl+Q = Nonsense\n") && copy.size() == m.size());  // bad file leaves the map alone
  CHECK(!copy.deserialize("garbage") && copy.size() == m.size());

  // Rebinding: a chord has one command; unbinding frees it.
  Keymap r = m;
  r.bind({Key::Space, 0}, Command::Record);
  CHECK(is(r, Key::Space, 0, Command::Record));
  r.unbind({Key::Space, 0});
  CHECK(!r.lookup({Key::Space, 0}));
  std::printf(failures ? "FAILED\n" : "command tests passed\n");
  return failures;
}
