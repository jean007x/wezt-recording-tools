#pragma once
// Commands and the keyboard map. The UI toolkit feeds in key chords (using our own Key enum, so
// this layer has no dependency on ImGui) and gets a Command back. The default map follows the
// published shortcut table of Pro Tools (Windows layout; on macOS Ctrl reads as Cmd and "Start"
// reads as Control). Only the key combinations are matched; the command names and the code are ours.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wezt::ui {

enum class Key : std::uint8_t {
  None, A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  Kp0, Kp1, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9, KpEnter,
  Space, Enter, Escape, Tab, Backquote, Equals, LeftBracket, RightBracket, Backspace, Delete,
  Left, Right, Up, Down, Home, End
};

enum Mod : std::uint8_t { kCtrl = 1, kShift = 2, kAlt = 4, kStart = 8 };  // kCtrl = Cmd on macOS, kStart = Control on macOS, kAlt = Option

struct Chord {
  Key key = Key::None;
  std::uint8_t mods = 0;
  bool operator==(const Chord& o) const { return key == o.key && mods == o.mods; }
};

enum class Command : std::uint16_t {
  // transport
  PlayStop, PlayHalfSpeed, Record, StopRecord, LoopPlayback, LoopRecord, ReturnToStart, GoToEnd,
  Rewind, FastForward, QuickPunch, Pause, OnlinePlayback, Metronome, CountOff, PreRoll, PunchRecord,
  // edit modes and tools
  ModeShuffle, ModeSlip, ModeSpot, ModeGrid, ModeSnapToGrid, CycleEditModes,
  ToolZoomer, ToolTrimmer, ToolSelector, ToolGrabber, ToolScrubber, ToolPencil, ToolSmart, CycleEditTools,
  // editing
  Undo, Redo, Cut, Copy, Paste, SpecialPaste, SpotDialog, TimeOperations, PinTrack, CaptureClip, ColorClip, Clear, SelectAll, Duplicate, RepeatSelection, SeparateClip, HealSeparation,
  ConsolidateClip, InsertSilence, StripSilence, MuteClips, TrimClipToSelection, TrimStartToInsertion,
  TrimEndToInsertion, RestoreSelection, CreateFades, DeleteSelection,
  // windows and zoom
  ToggleMixEdit, ShowTransport, ShowBigCounter, ShowMemoryLocations, ZoomIn, ZoomOut, ZoomSession,
  ZoomSelection, FillWindowWithSelection, ShowAllClips, ZoomToggle,
  // tracks
  NewTrack, AddTrack, RemoveTrack, DuplicateTrack, GroupTracks, CommitTrack, BounceTrack, ScrollToTrack,
  // file
  NewSession, OpenSession, CloseSession, SaveSession, SaveSessionAs, BounceMix, ImportAudio, OpenRecent,
  // Wezt additions
  OpenSampler, OpenImmersivePanner,
  Count_
};

struct CommandInfo {
  Command id;
  const char* name;      // short menu text
};

const char* commandName(Command c);
const std::vector<CommandInfo>& allCommands();

class Keymap {
 public:
  static Keymap protoolsDefaults();
  void bind(Chord c, Command cmd);                 // replaces any earlier binding of the chord
  void unbind(Chord c);
  std::optional<Command> lookup(Chord c) const;
  std::vector<Chord> chordsFor(Command cmd) const; // every key combination that triggers it
  // Text such as "Ctrl+Shift+N" (mac = true: "Cmd+Shift+N").
  static std::string describe(Chord c, bool mac = false);
  // Parses text produced by describe(); false if it is not understood.
  static bool parse(const std::string& text, Chord& out);
  // Rebinding persists as text, one "Chord = Command" per line.
  std::string serialize() const;
  bool deserialize(const std::string& text);
  std::size_t size() const { return entries_.size(); }

 private:
  struct Entry { Chord chord; Command cmd; };
  std::vector<Entry> entries_;
};

}  // namespace wezt::ui
