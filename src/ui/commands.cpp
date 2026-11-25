#include "ui/commands.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace wezt::ui {
namespace {

struct KeyName { Key k; const char* n; };
const KeyName kKeyNames[] = {
  {Key::A,"A"},{Key::B,"B"},{Key::C,"C"},{Key::D,"D"},{Key::E,"E"},{Key::F,"F"},{Key::G,"G"},{Key::H,"H"},{Key::I,"I"},
  {Key::J,"J"},{Key::K,"K"},{Key::L,"L"},{Key::M,"M"},{Key::N,"N"},{Key::O,"O"},{Key::P,"P"},{Key::Q,"Q"},{Key::R,"R"},
  {Key::S,"S"},{Key::T,"T"},{Key::U,"U"},{Key::V,"V"},{Key::W,"W"},{Key::X,"X"},{Key::Y,"Y"},{Key::Z,"Z"},
  {Key::D0,"0"},{Key::D1,"1"},{Key::D2,"2"},{Key::D3,"3"},{Key::D4,"4"},{Key::D5,"5"},{Key::D6,"6"},{Key::D7,"7"},{Key::D8,"8"},{Key::D9,"9"},
  {Key::F1,"F1"},{Key::F2,"F2"},{Key::F3,"F3"},{Key::F4,"F4"},{Key::F5,"F5"},{Key::F6,"F6"},{Key::F7,"F7"},{Key::F8,"F8"},
  {Key::F9,"F9"},{Key::F10,"F10"},{Key::F11,"F11"},{Key::F12,"F12"},
  {Key::Kp0,"Num0"},{Key::Kp1,"Num1"},{Key::Kp2,"Num2"},{Key::Kp3,"Num3"},{Key::Kp4,"Num4"},{Key::Kp5,"Num5"},{Key::Kp6,"Num6"},
  {Key::Kp7,"Num7"},{Key::Kp8,"Num8"},{Key::Kp9,"Num9"},{Key::KpEnter,"NumEnter"},
  {Key::Space,"Space"},{Key::Enter,"Enter"},{Key::Escape,"Esc"},{Key::Tab,"Tab"},{Key::Backquote,"`"},{Key::Equals,"="},
  {Key::LeftBracket,"["},{Key::RightBracket,"]"},{Key::Backspace,"Backspace"},{Key::Delete,"Delete"},
  {Key::Left,"Left"},{Key::Right,"Right"},{Key::Up,"Up"},{Key::Down,"Down"},{Key::Home,"Home"},{Key::End,"End"},
};

const CommandInfo kCommands[] = {
  {Command::PlayStop,"Play / Stop"},{Command::PlayHalfSpeed,"Play at Half Speed"},{Command::Record,"Record"},
  {Command::StopRecord,"Stop Record"},{Command::LoopPlayback,"Loop Playback"},{Command::LoopRecord,"Loop Record"},
  {Command::ReturnToStart,"Return to Start"},{Command::GoToEnd,"Go to End"},{Command::Rewind,"Rewind"},
  {Command::FastForward,"Fast Forward"},{Command::QuickPunch,"QuickPunch"},{Command::Pause,"Pause"},
  {Command::OnlinePlayback,"Online"},{Command::Metronome,"Metronome"},{Command::CountOff,"Count Off"},{Command::PreRoll,"Pre-roll"},{Command::PunchRecord,"Record Selection (Punch)"},
  {Command::ModeShuffle,"Shuffle"},{Command::ModeSlip,"Slip"},{Command::ModeSpot,"Spot"},{Command::ModeGrid,"Grid"},
  {Command::ModeSnapToGrid,"Snap to Grid"},{Command::CycleEditModes,"Cycle Edit Modes"},
  {Command::ToolZoomer,"Zoomer"},{Command::ToolTrimmer,"Trimmer"},{Command::ToolSelector,"Selector"},
  {Command::ToolGrabber,"Grabber"},{Command::ToolScrubber,"Scrubber"},{Command::ToolPencil,"Pencil"},
  {Command::ToolSmart,"Smart Tool"},{Command::CycleEditTools,"Cycle Edit Tools"},
  {Command::Undo,"Undo"},{Command::Redo,"Redo"},{Command::Cut,"Cut"},{Command::Copy,"Copy"},{Command::Paste,"Paste"},{Command::SpecialPaste,"Special Paste"},{Command::SpotDialog,"Spot Dialog"},
  {Command::Clear,"Clear"},{Command::SelectAll,"Select All"},{Command::Duplicate,"Duplicate"},
  {Command::RepeatSelection,"Repeat"},{Command::SeparateClip,"Separate Clip"},{Command::HealSeparation,"Heal Separation"},
  {Command::ConsolidateClip,"Consolidate Clip"},{Command::InsertSilence,"Insert Silence"},{Command::StripSilence,"Strip Silence"},
  {Command::MuteClips,"Mute / Unmute Clips"},{Command::TrimClipToSelection,"Trim Clip to Selection"},
  {Command::TrimStartToInsertion,"Trim Start to Insertion"},{Command::TrimEndToInsertion,"Trim End to Insertion"},
  {Command::RestoreSelection,"Restore Last Selection"},{Command::CreateFades,"Create Fades"},
  {Command::DeleteSelection,"Delete Selection"},
  {Command::ToggleMixEdit,"Switch Mix / Edit"},{Command::ShowTransport,"Show Transport"},{Command::ShowBigCounter,"Show Big Counter"},
  {Command::ShowMemoryLocations,"Show Memory Locations"},{Command::ZoomIn,"Zoom In"},{Command::ZoomOut,"Zoom Out"},
  {Command::ZoomSession,"Zoom to Session"},{Command::ZoomSelection,"Zoom to Selection"},
  {Command::FillWindowWithSelection,"Fill Window with Selection"},{Command::ShowAllClips,"Show All Clips"},{Command::ZoomToggle,"Zoom Toggle"},
  {Command::NewTrack,"New Track..."},{Command::AddTrack,"Add Track (+)"},{Command::RemoveTrack,"Remove Track (-)"},
  {Command::DuplicateTrack,"Duplicate Track"},{Command::GroupTracks,"Group Tracks"},{Command::CommitTrack,"Commit Track"},
  {Command::BounceTrack,"Bounce Track"},{Command::ScrollToTrack,"Scroll to Track"},
  {Command::NewSession,"New Session"},{Command::OpenSession,"Open Session"},{Command::CloseSession,"Close Session"},
  {Command::SaveSession,"Save Session"},{Command::SaveSessionAs,"Save Session As"},{Command::BounceMix,"Bounce Mix"},
  {Command::ImportAudio,"Import Audio"},{Command::OpenRecent,"Open Most Recent"},
  {Command::OpenSampler,"Sampler"},{Command::OpenImmersivePanner,"Immersive Panner"},
};

std::string upper(std::string s) { for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }

}  // namespace

const std::vector<CommandInfo>& allCommands() {
  static const std::vector<CommandInfo> v(std::begin(kCommands), std::end(kCommands));
  return v;
}

const char* commandName(Command c) {
  for (const auto& i : kCommands) if (i.id == c) return i.name;
  return "";
}

void Keymap::unbind(Chord c) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.chord == c; }), entries_.end());
}
void Keymap::bind(Chord c, Command cmd) { unbind(c); entries_.push_back({c, cmd}); }

std::optional<Command> Keymap::lookup(Chord c) const {
  for (const auto& e : entries_) if (e.chord == c) return e.cmd;
  return std::nullopt;
}

std::vector<Chord> Keymap::chordsFor(Command cmd) const {
  std::vector<Chord> v;
  for (const auto& e : entries_) if (e.cmd == cmd) v.push_back(e.chord);
  return v;
}

std::string Keymap::describe(Chord c, bool mac) {
  std::string s;
  if (c.mods & kStart) s += mac ? "Control+" : "Start+";
  if (c.mods & kCtrl) s += mac ? "Cmd+" : "Ctrl+";
  if (c.mods & kAlt) s += mac ? "Option+" : "Alt+";
  if (c.mods & kShift) s += "Shift+";
  for (const auto& k : kKeyNames) if (k.k == c.key) { s += k.n; return s; }
  return s + "?";
}

bool Keymap::parse(const std::string& text, Chord& out) {
  Chord c;
  std::string rest = text;
  for (;;) {
    const auto plus = rest.find('+');
    if (plus == std::string::npos || plus == 0 || plus + 1 >= rest.size()) break;
    const std::string m = upper(rest.substr(0, plus));
    if (m == "CTRL" || m == "CMD") c.mods |= kCtrl;
    else if (m == "SHIFT") c.mods |= kShift;
    else if (m == "ALT" || m == "OPTION") c.mods |= kAlt;
    else if (m == "START" || m == "CONTROL") c.mods |= kStart;
    else return false;
    rest = rest.substr(plus + 1);
  }
  for (const auto& k : kKeyNames) if (upper(k.n) == upper(rest)) { c.key = k.k; out = c; return true; }
  return false;
}

std::string Keymap::serialize() const {
  std::string s;
  for (const auto& e : entries_) s += describe(e.chord) + " = " + commandName(e.cmd) + "\n";
  return s;
}

bool Keymap::deserialize(const std::string& text) {
  Keymap fresh;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find(" = ");
    if (eq == std::string::npos) return false;
    Chord c;
    if (!parse(line.substr(0, eq), c)) return false;
    const std::string name = line.substr(eq + 3);
    bool found = false;
    for (const auto& i : kCommands) if (name == i.name) { fresh.bind(c, i.id); found = true; break; }
    if (!found) return false;
  }
  *this = std::move(fresh);
  return true;
}

Keymap Keymap::protoolsDefaults() {
  Keymap m;
  auto b = [&](Key k, std::uint8_t mods, Command c) { m.bind({k, mods}, c); };
  using C = Command;
  // Transport
  b(Key::Space, 0, C::PlayStop);
  b(Key::Space, kShift, C::PlayHalfSpeed);
  b(Key::Space, kCtrl, C::Record);
  b(Key::F12, 0, C::Record);
  b(Key::Kp3, 0, C::Record);
  b(Key::Space, kStart, C::Pause);
  b(Key::L, kCtrl | kShift, C::LoopPlayback);
  b(Key::L, kAlt, C::LoopRecord);
  b(Key::J, kCtrl, C::OnlinePlayback);
  b(Key::Space, kAlt, C::OnlinePlayback);
  b(Key::Enter, 0, C::ReturnToStart);
  b(Key::Enter, kCtrl, C::GoToEnd);
  b(Key::Kp0, 0, C::PlayStop);
  b(Key::Kp1, 0, C::Rewind);
  b(Key::Kp2, 0, C::FastForward);
  b(Key::Kp4, 0, C::LoopPlayback);
  b(Key::Kp5, 0, C::LoopRecord);
  b(Key::Kp6, 0, C::QuickPunch);
  b(Key::Kp7, 0, C::Metronome);
  b(Key::Kp8, 0, C::CountOff);
  b(Key::Kp9, 0, C::PreRoll);
  b(Key::R, kCtrl | kShift, C::PunchRecord);
  // Edit modes and tools
  b(Key::F1, 0, C::ModeShuffle);  b(Key::D1, kAlt, C::ModeShuffle);
  b(Key::F2, 0, C::ModeSlip);     b(Key::D2, kAlt, C::ModeSlip);
  b(Key::F3, 0, C::ModeSpot);     b(Key::D3, kAlt, C::ModeSpot);
  b(Key::F4, 0, C::ModeGrid);     b(Key::D4, kAlt, C::ModeGrid);
  b(Key::F4, kShift, C::ModeSnapToGrid);
  b(Key::Backquote, 0, C::CycleEditModes);
  b(Key::F5, 0, C::ToolZoomer);    b(Key::D1, kCtrl, C::ToolZoomer);
  b(Key::F6, 0, C::ToolTrimmer);   b(Key::D2, kCtrl, C::ToolTrimmer);
  b(Key::F7, 0, C::ToolSelector);  b(Key::D3, kCtrl, C::ToolSelector);
  b(Key::F8, 0, C::ToolGrabber);   b(Key::D4, kCtrl, C::ToolGrabber);
  b(Key::F9, 0, C::ToolScrubber);  b(Key::D5, kCtrl, C::ToolScrubber);
  b(Key::F10, 0, C::ToolPencil);   b(Key::D6, kCtrl, C::ToolPencil);
  b(Key::D7, kCtrl, C::ToolSmart);
  b(Key::Escape, 0, C::CycleEditTools);
  // Editing
  b(Key::Z, kCtrl, C::Undo);
  b(Key::Z, kCtrl | kShift, C::Redo);
  b(Key::X, kCtrl, C::Cut);
  b(Key::C, kCtrl, C::Copy);
  b(Key::V, kCtrl, C::Paste);
  b(Key::V, kCtrl | kShift, C::SpecialPaste);
  b(Key::F3, kShift, C::SpotDialog);
  b(Key::B, kCtrl, C::Clear);
  b(Key::A, kCtrl, C::SelectAll);
  b(Key::D, kCtrl, C::Duplicate);
  b(Key::R, kAlt, C::RepeatSelection);
  b(Key::E, kCtrl, C::SeparateClip);
  b(Key::H, kCtrl, C::HealSeparation);
  b(Key::D3, kAlt | kShift, C::ConsolidateClip);
  b(Key::E, kCtrl | kShift, C::InsertSilence);
  b(Key::U, kCtrl, C::StripSilence);
  b(Key::M, kCtrl, C::MuteClips);
  b(Key::T, kCtrl, C::TrimClipToSelection);
  b(Key::D7, kAlt | kShift, C::TrimStartToInsertion);
  b(Key::D8, kAlt | kShift, C::TrimEndToInsertion);
  b(Key::Z, kCtrl | kAlt, C::RestoreSelection);
  b(Key::F, kCtrl, C::CreateFades);
  b(Key::Backspace, 0, C::DeleteSelection);
  b(Key::Delete, 0, C::DeleteSelection);
  // Windows and zoom
  b(Key::Equals, kCtrl, C::ToggleMixEdit);
  b(Key::Kp1, kCtrl, C::ShowTransport);
  b(Key::Kp3, kCtrl, C::ShowBigCounter);
  b(Key::Kp5, kCtrl, C::ShowMemoryLocations);
  b(Key::RightBracket, kCtrl, C::ZoomIn);
  b(Key::LeftBracket, kCtrl, C::ZoomOut);
  b(Key::LeftBracket, kCtrl | kStart, C::ZoomSession);
  b(Key::RightBracket, kCtrl | kStart, C::ZoomSelection);
  b(Key::F, kAlt, C::FillWindowWithSelection);
  b(Key::A, kAlt, C::ShowAllClips);
  b(Key::E, kStart, C::ZoomToggle);
  // Tracks
  b(Key::N, kCtrl | kShift, C::NewTrack);
  b(Key::D, kAlt | kShift, C::DuplicateTrack);
  b(Key::G, kCtrl, C::GroupTracks);
  b(Key::C, kAlt | kShift, C::CommitTrack);
  b(Key::B, kCtrl | kAlt | kShift, C::BounceTrack);
  b(Key::F, kCtrl | kAlt, C::ScrollToTrack);
  // File
  b(Key::N, kCtrl, C::NewSession);
  b(Key::O, kCtrl, C::OpenSession);
  b(Key::O, kCtrl | kShift, C::OpenRecent);
  b(Key::W, kCtrl | kShift, C::CloseSession);
  b(Key::S, kCtrl, C::SaveSession);
  b(Key::S, kCtrl | kStart, C::SaveSessionAs);
  b(Key::B, kCtrl | kAlt, C::BounceMix);
  b(Key::I, kCtrl | kShift, C::ImportAudio);
  // Wezt additions (no Pro Tools equivalent)
  b(Key::S, kAlt | kShift, C::OpenSampler);
  b(Key::P, kAlt | kShift, C::OpenImmersivePanner);
  // "Add track" / "Remove track" are the +/- buttons; the keys below are our own.
  b(Key::Equals, kCtrl | kShift, C::AddTrack);
  b(Key::Delete, kCtrl | kShift, C::RemoveTrack);
  return m;
}

}  // namespace wezt::ui
