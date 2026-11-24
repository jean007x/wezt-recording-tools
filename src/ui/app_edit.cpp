// Toolbar, overview, tracks/groups list and the Edit window (rulers, track headers, clips).
#include <algorithm>
#include <optional>
#include <cmath>
#include <cstdio>

#include <limits>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "ui/app.h"
#include "ui/style.h"

namespace wezt::ui {
using session::Clip;
using session::TrackId;
namespace st = style;

namespace {
ImU32 col(float r, float g, float b, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }

std::string timecode(std::int64_t frames, double rate) {
  const double sec = static_cast<double>(frames) / rate;
  const int m = static_cast<int>(sec / 60.0);
  char b[48];
  std::snprintf(b, sizeof b, "%d:%02d.%03d", m, static_cast<int>(std::fmod(sec, 60.0)), static_cast<int>(std::fmod(sec * 1000.0, 1000.0)));
  return b;
}
std::string barsBeats(std::int64_t frames, double rate, float tempo, int perBar = 4) {
  const double beats = static_cast<double>(frames) / (rate * 60.0 / static_cast<double>(tempo));
  char b[48];
  std::snprintf(b, sizeof b, "%d| %d| %03d", 1 + static_cast<int>(beats / perBar), 1 + static_cast<int>(std::fmod(beats, static_cast<double>(perBar))), static_cast<int>(std::fmod(beats, 1.0) * 960.0));
  return b;
}
std::string clipName(const Clip& c, const std::string& fallback) {
  std::string n = c.file;
  const auto s = n.find_last_of("/\\");
  if (s != std::string::npos) n = n.substr(s + 1);
  const auto d = n.find_last_of('.');
  if (d != std::string::npos && d > 0) n = n.substr(0, d);
  return n.empty() ? fallback : n;
}
const ImU32 kGreenOn = 0xFF50E08C, kGreenBox = 0xFF2A4A1C, kGreenTxt = 0xFF8CE0A0;
}  // namespace

// ---------------------------------------------------------------------------------------------
// groups, markers, small actions
template <class F> void App::forGroup(TrackId t, F f) {
  std::set<TrackId> who{t};
  if (allGroupActive_) for (TrackId id : session_->trackIds()) who.insert(id);
  for (const TrackGroup& g : groups_) if (g.active && std::find(g.members.begin(), g.members.end(), t) != g.members.end()) who.insert(g.members.begin(), g.members.end());
  for (TrackId id : who) if (session::Track* tr = session_->track(id)) f(id, tr);
}
void App::setMuteGrouped(TrackId t, bool on) { forGroup(t, [&](TrackId, session::Track* tr) { tr->strip->setMute(on); }); }
void App::setSoloGrouped(TrackId t, bool on) { forGroup(t, [&](TrackId id, session::Track*) { session_->setSolo(id, on); }); }
void App::setArmGrouped(TrackId t, bool on) { forGroup(t, [&](TrackId, session::Track* tr) { tr->source->arm(on); }); }

int App::addMarker(std::int64_t frame, const std::string& name) {
  Marker m; m.frame = std::max<std::int64_t>(0, frame);
  m.name = name.empty() ? "Marker " + std::to_string(markers_.size() + 1) : name;
  markers_.push_back(m);
  std::sort(markers_.begin(), markers_.end(), [](const Marker& a, const Marker& b) { return a.frame < b.frame; });
  for (std::size_t i = 0; i < markers_.size(); ++i) if (markers_[i].frame == m.frame && markers_[i].name == m.name) return static_cast<int>(i);
  return 0;
}
int App::addGroup(const std::string& name, const std::vector<TrackId>& members) {
  TrackGroup g; g.letter = static_cast<char>('a' + (groups_.size() % 26)); g.name = name; g.members = members;
  groups_.push_back(g);
  return static_cast<int>(groups_.size() - 1);
}
void App::drawBigCounter() {
  ImGui::SetNextWindowSize(ImVec2(520, 150), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Big Counter", &showBigCounter_)) { ImGui::End(); return; }
  const double rate = session_->sampleRate();
  const std::int64_t pos = position();
  const std::string main = timecode_ ? barsBeats(pos, rate, tempo_, beatsPerBar_) : timecode(pos, rate);
  const float avail = ImGui::GetContentRegionAvail().x;
  const float base = ImGui::GetFontSize() * static_cast<float>(main.size()) * 0.55f;
  ImGui::SetWindowFontScale(std::clamp(avail / std::max(1.f, base), 1.f, 8.f));
  ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.35f, 1.f, 0.45f));
  ImGui::TextUnformatted(main.c_str());
  ImGui::PopStyleColor();
  ImGui::SetWindowFontScale(1.f);
  if (ImGui::IsItemClicked()) timecode_ = !timecode_;
  ImGui::End();
}

void App::nudgeSelectedClip(int direction) {
  session::Track* t = session_->track(selTrack_);
  if (!t || selClip_ < 0) { setStatus("Select a clip to nudge"); return; }
  auto list = t->source->playlist();
  for (Clip& c : list) if (c.id == selClip_) c.start = std::max<std::int64_t>(0, c.start + direction * nudgeFrames());
  t->source->setPlaylist(std::move(list));
}
void App::storeZoomPreset(int slot) { if (slot >= 0 && slot < 5) zoomPresets_[slot] = {true, framesPerPixel_, scrollFrames_}; }
bool App::recallZoomPreset(int slot) {
  if (slot < 0 || slot >= 5 || !zoomPresets_[slot].set) return false;
  framesPerPixel_ = zoomPresets_[slot].fpp; scrollFrames_ = zoomPresets_[slot].scroll;
  return true;
}

// ---------------------------------------------------------------------------------------------
void App::drawTitle() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  st::vgrad(dl, p, ImVec2(p.x + s.x, p.y + s.y), col(0.24f, 0.245f, 0.26f), col(0.17f, 0.175f, 0.19f));
  dl->AddLine(ImVec2(p.x, p.y + s.y - 1), ImVec2(p.x + s.x, p.y + s.y - 1), col(0.f, 0.f, 0.f));
  const std::string t = std::string(view_ == MainView::Edit ? "Edit: " : "Mix: ") + sessionName_;
  const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
  dl->AddText(ImVec2(p.x + (s.x - ts.x) * 0.5f, p.y + (s.y - ts.y) * 0.5f), col(0.86f, 0.87f, 0.9f), t.c_str());
}

void App::drawTransport() {
  auto& tr = engine_->transport();
  ImDrawList* bgl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  st::vgrad(bgl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), col(0.20f, 0.205f, 0.22f), col(0.125f, 0.128f, 0.14f));
  bgl->AddLine(ImVec2(wp.x, wp.y + ws.y - 1), ImVec2(wp.x + ws.x, wp.y + ws.y - 1), col(0.f, 0.f, 0.f));
  const ImU32 blue = 0xFFD2882F, green = 0xFF3FA84A, red = 0xFF2F2FD8, amber = 0xFF1E9AE6;
  auto caption = [](const char* t) { ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.58f, 0.60f, 0.65f)); ImGui::TextUnformatted(t); ImGui::PopStyleColor(); };
  const ImVec2 base(8.f, 6.f);

  // ---- edit modes: a dark green box with four buttons; the active one is lit
  {
    ImGui::SetCursorPos(base);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + 142.f, p.y + 46.f);
    bgl->AddRectFilled(p, q, kGreenBox, 3.f);
    bgl->AddRect(p, q, col(0.f, 0.f, 0.f), 3.f);
    struct M { const char* n; EditMode m; int cx, cy; };
    const M ms[] = {{"SHUFFLE", EditMode::Shuffle, 0, 0}, {"SPOT", EditMode::Spot, 1, 0}, {"SLIP", EditMode::Slip, 0, 1}, {"GRID", EditMode::Grid, 1, 1}};
    for (const M& m : ms) {
      const ImVec2 a(p.x + 3.f + static_cast<float>(m.cx) * 69.f, p.y + 3.f + static_cast<float>(m.cy) * 21.f), b(a.x + 66.f, a.y + 19.f);
      ImGui::SetCursorScreenPos(a);
      const bool isGrid = m.m == EditMode::Grid;
      if (ImGui::InvisibleButton(m.n, ImVec2(isGrid ? 50.f : 66.f, 19.f))) execute(m.m == EditMode::Shuffle ? Command::ModeShuffle : m.m == EditMode::Spot ? Command::ModeSpot : m.m == EditMode::Slip ? Command::ModeSlip : Command::ModeGrid);
      const bool on = mode_ == m.m;
      bgl->AddRectFilled(a, b, on ? kGreenOn : col(0.11f, 0.2f, 0.12f), 2.f);
      if (on) bgl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), st::shade(kGreenOn, 1.f, 90), 3.f, 0, 2.f);
      const ImVec2 ts = ImGui::CalcTextSize(m.n);
      bgl->AddText(ImVec2(a.x + (isGrid ? 25.f : 33.f) - ts.x * 0.5f, a.y + 2.f), on ? 0xFF10200C : kGreenTxt, m.n);
      if (isGrid) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + 50.f, a.y));
        if (ImGui::InvisibleButton("##gridsel", ImVec2(16.f, 19.f))) ImGui::OpenPopup("##gridpop");
        bgl->AddTriangleFilled(ImVec2(a.x + 53.f, a.y + 7.f), ImVec2(a.x + 61.f, a.y + 7.f), ImVec2(a.x + 57.f, a.y + 13.f), on ? 0xFF10200C : kGreenTxt);
      }
    }
    if (ImGui::BeginPopup("##gridpop")) {
      for (int i = 0; i < 5; ++i) if (ImGui::Selectable(kGridNames[i], gridIdx_ == i)) gridIdx_ = i;
      ImGui::EndPopup();
    }
  }
  // ---- zoom presets 1-5
  ImGui::SetCursorPos(ImVec2(base.x + 152.f, base.y));
  ImGui::BeginGroup();
  caption("ZOOM PRESETS");
  for (int i = 0; i < 5; ++i) {
    if (i) ImGui::SameLine(0, 2);
    char lb[4]; std::snprintf(lb, sizeof lb, "%d", i + 1);
    if (st::chip(lb, zoomPresets_[i].set, blue, ImVec2(24, 22))) {
      if (ImGui::GetIO().KeyShift || !zoomPresets_[i].set) storeZoomPreset(i); else recallZoomPreset(i);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(zoomPresets_[i].set ? "Click: recall this zoom.  Shift+click: store the current zoom here" : "Click: store the current zoom here");
  }
  ImGui::EndGroup();
  // ---- zoom cluster
  ImGui::SameLine(0, 14);
  ImGui::BeginGroup();
  caption("ZOOM");
  if (st::toolButton("##zl", st::TIcon::Left, false, blue, ImVec2(24, 22))) execute(Command::ZoomOut);
  ImGui::SameLine(0, 2);
  if (st::toolButton("##zr", st::TIcon::Right, false, blue, ImVec2(24, 22))) execute(Command::ZoomIn);
  ImGui::SameLine(0, 2);
  if (st::toolButton("##zw", st::TIcon::Wave, waveScale_ > 1.f, blue, ImVec2(26, 22))) waveScale_ = waveScale_ >= 4.f ? 1.f : waveScale_ * 2.f;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Waveform height: click to cycle 1x, 2x, 4x");
  ImGui::SameLine(0, 2);
  if (st::toolButton("##zb", st::TIcon::Bars, false, blue, ImVec2(26, 22))) trackLevel_ = (trackLevel_ + 1) % 3;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Track height: click to cycle small, medium, large");
  ImGui::EndGroup();
  // ---- tools
  ImGui::SameLine(0, 14);
  ImGui::BeginGroup();
  caption("TOOLS");
  struct T { const char* id; st::TIcon ic; Tool t; Command c; const char* tip; };
  const T tools[] = {{"##tsm", st::TIcon::Smart, Tool::Smart, Command::ToolSmart, "Smart tool"}, {"##tz", st::TIcon::Zoom, Tool::Zoomer, Command::ToolZoomer, "Zoomer"},
                     {"##tt", st::TIcon::Trim, Tool::Trimmer, Command::ToolTrimmer, "Trimmer"}, {"##ts", st::TIcon::Selector, Tool::Selector, Command::ToolSelector, "Selector"},
                     {"##tg", st::TIcon::Grabber, Tool::Grabber, Command::ToolGrabber, "Grabber"}};
  for (int i = 0; i < 5; ++i) {
    if (i) ImGui::SameLine(0, i == 2 ? 8 : 2);
    if (st::toolButton(tools[i].id, tools[i].ic, tool_ == tools[i].t, blue, ImVec2(30, 22))) execute(tools[i].c);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tools[i].tip);
  }
  ImGui::SameLine(0, 8);
  st::toolButton("##tscrub", st::TIcon::Scrub, false, blue, ImVec2(30, 22), false);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Scrub: not built yet");
  ImGui::SameLine(0, 2);
  st::toolButton("##tpen", st::TIcon::Pencil, false, blue, ImVec2(30, 22), false);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Pencil: not built yet");
  ImGui::EndGroup();

  // ---- second row: transport, follow/loop, tracks (+ - Dup), markers, window
  ImGui::SetCursorPos(ImVec2(base.x, base.y + 52.f));
  const ImVec2 bs(34, 26);
  if (st::iconButton("##start", st::Icon::ToStart, false, 0, bs)) execute(Command::ReturnToStart);
  ImGui::SameLine(0, 2);
  if (st::iconButton("##rew", st::Icon::Rewind, false, 0, bs)) execute(Command::Rewind);
  ImGui::SameLine(0, 2);
  if (st::iconButton("##play", tr.playing() ? st::Icon::Stop : st::Icon::Play, tr.playing(), green, ImVec2(42, 26))) execute(Command::PlayStop);
  ImGui::SameLine(0, 2);
  if (st::iconButton("##ff", st::Icon::Forward, false, 0, bs)) execute(Command::FastForward);
  ImGui::SameLine(0, 2);
  if (st::iconButton("##rec", st::Icon::Record, tr.recording(), red, bs)) execute(Command::Record);
  ImGui::SameLine(0, 2);
  if (st::iconButton("##loop", st::Icon::Loop, tr.looping(), amber, bs)) execute(Command::LoopPlayback);
  ImGui::SameLine(0, 10);
  if (st::toolButton("##follow", st::TIcon::Follow, followPlayhead_, blue, ImVec2(30, 26))) followPlayhead_ = !followPlayhead_;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scroll with the playhead");
  ImGui::SameLine(0, 2);
  if (st::toolButton("##link", st::TIcon::Link, true, blue, ImVec2(30, 26), false)) {}
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Timeline and edit selection are always linked here");
  ImGui::SameLine(0, 14);
  caption("TRACKS"); ImGui::SameLine(0, 6);
  if (st::chip("+", false, blue, ImVec2(28, 26))) execute(Command::AddTrack);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a track");
  ImGui::SameLine(0, 2);
  if (st::chip("-", false, blue, ImVec2(28, 26))) execute(Command::RemoveTrack);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the selected track");
  ImGui::SameLine(0, 2);
  if (st::chip("Dup", false, blue, ImVec2(40, 26))) execute(Command::DuplicateTrack);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate the selected track with its clips, plug-ins and sends");
  ImGui::SameLine(0, 14);
  caption("MARKER"); ImGui::SameLine(0, 6);
  if (st::toolButton("##addmk", st::TIcon::Marker, false, blue, ImVec2(30, 26))) addMarker(position());
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a marker at the playhead");
  ImGui::SameLine(0, 14);
  if (st::chip("Edit", view_ == MainView::Edit, blue, ImVec2(46, 26))) view_ = MainView::Edit;
  ImGui::SameLine(0, 2);
  if (st::chip("Mix", view_ == MainView::Mix, blue, ImVec2(46, 26))) view_ = MainView::Mix;
  ImGui::SameLine(0, 2);
  if (st::chip("Float mix", showMixFloat_, blue, ImVec2(76, 26))) showMixFloat_ = !showMixFloat_;
  ImGui::SameLine(0, 14);
  caption("TEMPO"); ImGui::SameLine(0, 6);
  ImGui::SetNextItemWidth(64);
  ImGui::DragFloat("##tempo", &tempo_, 0.5f, 30.f, 300.f, "%.1f");
  ImGui::SameLine(0, 6);
  ImGui::SetNextItemWidth(52);
  ImGui::DragInt("##sig", &beatsPerBar_, 0.1f, 1, 12, "%d/4");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Beats per bar (time signature, quarter-note beats): drag to change");
  ImGui::SameLine(0, 6);
  if (st::chip("Click", metronome_, blue, ImVec2(46, 26))) execute(Command::Metronome);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Metronome: a click on every beat while playing (heard, never recorded)");
  ImGui::SameLine(0, 2);
  {
    const int cb = engine_->metronome().countOffBars();
    const std::string lbl = cb == 0 ? "Count" : "Count " + std::to_string(cb);
    if (st::chip((lbl + "##cnt").c_str(), cb > 0 || engine_->countingOff(), blue, ImVec2(66, 26))) execute(Command::CountOff);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Count-off: bars of clicks before recording starts (click to change: off, 1, 2, 4)");
  }

  ImGui::SameLine(0, 2);
  {
    const int ps = static_cast<int>(std::lround(static_cast<double>(engine_->preRollFrames()) / session_->sampleRate()));
    const std::string lbl = ps == 0 ? "Pre" : "Pre " + std::to_string(ps) + "s";
    if (st::chip((lbl + "##pre").c_str(), ps > 0 || engine_->preRolling(), blue, ImVec2(60, 26))) execute(Command::PreRoll);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pre-roll: when recording from the playhead, play in from a few seconds earlier (click to change: off, 2, 4, 8 s)");
  }

  // ---- counters at the right
  const double rate = session_->sampleRate();
  const std::int64_t pos = position();
  const std::int64_t a = selEnd_ > selStart_ ? selStart_ : pos, b = selEnd_ > selStart_ ? selEnd_ : pos;
  const float rightW = 700.f;
  float x = std::max(ImGui::GetCursorPosX() + 12.f, ws.x - rightW - 8.f);
  if (x + rightW > ws.x) x = std::max(8.f, ws.x - rightW - 8.f);
  ImGui::SetCursorPos(ImVec2(x, 6.f));
  ImGui::BeginGroup();
  const std::string main = timecode_ ? barsBeats(pos, rate, tempo_, beatsPerBar_) : timecode(pos, rate);
  st::lcd(main.c_str(), "000| 0| 000", st::rgba(0.35f, 1.f, 0.45f), 2.0f);
  if (ImGui::IsItemClicked()) timecode_ = !timecode_;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to swap the main and sub counters");
  ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.40f, 0.85f, 0.50f));
  ImGui::TextUnformatted((timecode_ ? timecode(pos, rate) : barsBeats(pos, rate, tempo_, beatsPerBar_)).c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 10); caption("Cursor");
  ImGui::EndGroup();
  ImGui::SameLine(0, 14);
  ImGui::BeginGroup();
  auto row = [&](const char* label, std::int64_t f) {
    caption(label); ImGui::SameLine(46);
    ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.55f, 0.95f, 0.62f));
    ImGui::TextUnformatted(barsBeats(f, rate, tempo_, beatsPerBar_).c_str());
    ImGui::PopStyleColor();
  };
  row("Start", a); row("End", b); row("Length", b - a);
  ImGui::EndGroup();
  ImGui::SameLine(0, 14);
  ImGui::BeginGroup();
  caption("MIDI In");
  caption("MIDI Out");
  ImGui::EndGroup();
  ImGui::SameLine(0, 4);
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool inLit = hwActivityAt_ >= 0.0 && ImGui::GetTime() - hwActivityAt_ < 0.15;   // MIDI In: a keyboard message in the last 150 ms
    bgl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + 12, p.y + 12), inLit ? col(0.35f, 0.9f, 0.4f) : col(0.16f, 0.17f, 0.19f), 2.f);
    bgl->AddRectFilled(ImVec2(p.x, p.y + 22), ImVec2(p.x + 12, p.y + 30), col(0.16f, 0.17f, 0.19f), 2.f);
    ImGui::Dummy(ImVec2(14, 30));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(hwMidi_ ? "MIDI In lights on keyboard messages. MIDI Out is not built." : "No MIDI keyboard connected (choose one in the Piano Roll). MIDI Out is not built.");
  }
  ImGui::SameLine(0, 14);
  ImGui::BeginGroup();
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bgl->AddRectFilled(p, ImVec2(p.x + 166, p.y + 46), kGreenBox, 3.f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 4, p.y + 3));
    if (st::chip("Grid", mode_ == EditMode::Grid, kGreenOn, ImVec2(44, 19), 0xFF10200C)) execute(Command::ModeGrid);
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(94);
    if (ImGui::BeginCombo("##gridv", kGridNames[gridIdx_])) {
      for (int i = 0; i < 5; ++i) if (ImGui::Selectable(kGridNames[i], gridIdx_ == i)) gridIdx_ = i;
      ImGui::EndCombo();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 4, p.y + 24));
    ImGui::PushStyleColor(ImGuiCol_Text, kGreenTxt);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Nudge");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(66);
    if (ImGui::BeginCombo("##nudgev", kNudgeNames[nudgeIdx_])) {
      for (int i = 0; i < 5; ++i) if (ImGui::Selectable(kNudgeNames[i], nudgeIdx_ == i)) nudgeIdx_ = i;
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, 2);
    if (st::chip("<", false, kGreenOn, ImVec2(18, 20))) nudgeSelectedClip(-1);
    ImGui::SameLine(0, 1);
    if (st::chip(">", false, kGreenOn, ImVec2(18, 20))) nudgeSelectedClip(1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge the selected clip by the nudge value");
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 48));
  }
  ImGui::EndGroup();
}

// ---------------------------------------------------------------------------------------------
void App::drawOverview() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  const ImVec2 a(p.x + 6.f, p.y + 3.f), b(p.x + s.x - 6.f, p.y + s.y - 3.f);
  dl->AddRectFilled(a, b, col(0.06f, 0.065f, 0.075f), 2.f);
  const std::int64_t end = std::max<std::int64_t>(endOfSession(), static_cast<std::int64_t>(session_->sampleRate() * 10.0));
  const double total = static_cast<double>(end) * 1.05;
  const auto ids = session_->trackIds();
  const float w = b.x - a.x, h = b.y - a.y;
  auto fx = [&](double f) { return a.x + static_cast<float>(f / total) * w; };
  const float rowH = ids.empty() ? h : std::max(1.5f, h / static_cast<float>(ids.size()));
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const session::Track* t = session_->track(ids[i]);
    if (!t) continue;
    const float y = a.y + static_cast<float>(i) * rowH;
    if (y + rowH > b.y + 0.5f) break;
    for (const Clip& c : t->source->playlist())
      dl->AddRectFilled(ImVec2(fx(static_cast<double>(c.start)), y + 0.5f), ImVec2(std::max(fx(static_cast<double>(c.start + c.length)), fx(static_cast<double>(c.start)) + 1.f), y + std::max(1.f, rowH - 0.5f)), st::trackColor(ids[i]));
  }
  const double vis = static_cast<double>(viewWidthPx_) * static_cast<double>(framesPerPixel_);
  const ImVec2 va(fx(scrollFrames_), a.y), vb(std::min(b.x, fx(scrollFrames_ + vis)), b.y);
  dl->AddRectFilled(va, vb, col(1.f, 1.f, 1.f, 0.16f));
  dl->AddRect(va, vb, col(1.f, 1.f, 1.f, 0.55f));
  dl->AddLine(ImVec2(fx(static_cast<double>(position())), a.y), ImVec2(fx(static_cast<double>(position())), b.y), col(1.f, 0.35f, 0.3f));
  ImGui::SetCursorScreenPos(a);
  ImGui::InvisibleButton("##ovw", ImVec2(w, h));
  if (ImGui::IsItemActive()) {
    const double f = static_cast<double>((ImGui::GetIO().MousePos.x - a.x) / w) * total;
    scrollFrames_ = std::max(0.0, f - vis * 0.5);
  }
}

// ---------------------------------------------------------------------------------------------
void App::drawSidebar(float height) {
  const auto ids = session_->trackIds();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, col(0.115f, 0.12f, 0.13f));
  const float groupsH = std::min(150.f, 40.f + 20.f * static_cast<float>(groups_.size() + 1));
  ImGui::BeginChild("##tracklist", ImVec2(0, height - groupsH - 8.f), ImGuiChildFlags_None);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();   // the child's own list, so the rows sit above its background
    ImGui::SetCursorPosX(8.f);
    ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.62f, 0.64f, 0.70f));
    ImGui::TextUnformatted("TRACKS");
    ImGui::PopStyleColor();
    for (TrackId id : ids) {
      const session::Track* t = session_->track(id);
      if (!t) continue;
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      const bool sel = id == selTrack_, hid = trackHidden(id);
      ImGui::PushID(id);
      if (ImGui::InvisibleButton("##row", ImVec2(w, 19.f))) selTrack_ = id;
      const bool rowHover = ImGui::IsItemHovered();
      if (sel) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 19.f), col(0.16f, 0.38f, 0.70f));
      else if (rowHover) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 19.f), col(1.f, 1.f, 1.f, 0.06f));
      dl->AddCircleFilled(ImVec2(p.x + 8.f, p.y + 9.5f), 3.f, hid ? col(0.35f, 0.36f, 0.4f) : col(0.85f, 0.88f, 0.92f));
      dl->AddRectFilled(ImVec2(p.x + 17.f, p.y + 4.f), ImVec2(p.x + 28.f, p.y + 15.f), st::trackColor(id), 2.f);
      dl->AddText(ImVec2(p.x + 34.f, p.y + 2.f), hid ? col(0.5f, 0.52f, 0.56f) : col(0.88f, 0.9f, 0.93f), t->name.c_str());
      ImGui::SetCursorScreenPos(p);
      if (ImGui::InvisibleButton("##vis", ImVec2(16.f, 19.f))) setTrackHidden(id, !hid);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(hid ? "Show this track" : "Hide this track");
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 20.f));
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  ImGui::BeginChild("##grouplist", ImVec2(0, groupsH), ImGuiChildFlags_None);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorPosX(8.f);
    ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.62f, 0.64f, 0.70f));
    ImGui::TextUnformatted("GROUPS");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40.f);
    if (ImGui::SmallButton("+")) {
      std::vector<TrackId> m;
      if (session_->track(selTrack_)) m.push_back(selTrack_);
      addGroup("GROUP " + std::to_string(groups_.size() + 1), m);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New group with the selected track");
    auto row = [&](int index, const std::string& label, bool active) {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      ImGui::PushID(index + 1000);
      const bool clicked = ImGui::InvisibleButton("##g", ImVec2(w, 19.f));
      if (active) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 19.f), col(0.16f, 0.38f, 0.70f));
      else if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 19.f), col(1.f, 1.f, 1.f, 0.06f));
      dl->AddText(ImVec2(p.x + 6.f, p.y + 2.f), col(0.88f, 0.9f, 0.93f), label.c_str());
      if (index >= 0 && ImGui::BeginPopupContextItem("##gm")) {
        TrackGroup& g = groups_[static_cast<std::size_t>(index)];
        if (ImGui::MenuItem("Add selected track") && std::find(g.members.begin(), g.members.end(), selTrack_) == g.members.end()) g.members.push_back(selTrack_);
        if (ImGui::MenuItem("Remove selected track")) g.members.erase(std::remove(g.members.begin(), g.members.end(), selTrack_), g.members.end());
        if (ImGui::MenuItem("Delete group")) { groups_.erase(groups_.begin() + index); ImGui::EndPopup(); ImGui::PopID(); return clicked ? 2 : 1; }
        ImGui::EndPopup();
      }
      ImGui::PopID();
      return clicked ? 1 : 0;
    };
    if (row(-1, "! <ALL>", allGroupActive_) == 1) allGroupActive_ = !allGroupActive_;
    for (std::size_t i = 0; i < groups_.size(); ++i) {
      const int r = row(static_cast<int>(i), std::string(1, groups_[i].letter) + " " + groups_[i].name + " (" + std::to_string(groups_[i].members.size()) + ")", groups_[i].active);
      if (r == 2) break;
      if (r == 1) groups_[i].active = !groups_[i].active;
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------------------------
void App::drawBottomTabs() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  st::vgrad(dl, p, ImVec2(p.x + s.x, p.y + s.y), col(0.17f, 0.175f, 0.19f), col(0.11f, 0.115f, 0.125f));
  ImGui::SetCursorPos(ImVec2(8.f, 3.f));
  const ImVec2 c(p.x + 18.f, p.y + s.y * 0.5f);
  dl->PathArcTo(c, 6.f, 0.5f, 5.6f, 14); dl->PathStroke(col(0.75f, 0.77f, 0.82f), 0, 1.8f);
  dl->AddTriangleFilled(ImVec2(c.x + 7.f, c.y - 4.f), ImVec2(c.x + 1.f, c.y - 4.f), ImVec2(c.x + 5.f, c.y + 1.f), col(0.75f, 0.77f, 0.82f));
  ImGui::SetCursorPos(ImVec2(36.f, 3.f));
  const ImU32 blue = 0xFFD2882F;
  if (st::chip("PIANO ROLL", showPianoRoll_, blue, ImVec2(92, 22))) showPianoRoll_ = !showPianoRoll_;
  ImGui::SameLine(0, 2);
  if (st::chip("SCORE", showScore_, blue, ImVec2(70, 22))) showScore_ = !showScore_;
  ImGui::SameLine(0, 2);
  if (st::chip("CLIPS", showClips_, blue, ImVec2(70, 22))) showClips_ = !showClips_;
  ImGui::SameLine(0, 2);
  if (st::chip("SAMPLER", showSampler_, blue, ImVec2(92, 22))) showSampler_ = !showSampler_;
  ImGui::SameLine(0, 2);
  if (st::chip("IMMERSIVE", showImmersive_, blue, ImVec2(92, 22))) showImmersive_ = !showImmersive_;
  ImGui::SameLine(0, 2);
  if (st::chip("CLIP FX", showClipFx_, blue, ImVec2(80, 22))) showClipFx_ = !showClipFx_;
}

// ---------------------------------------------------------------------------------------------
void App::drawEdit() {
  const float headerW = 232.f, rowRuler = 18.f, rulerH = rowRuler * 3.f;
  static const float levelH[3] = {54.f, 98.f, 140.f};
  const float laneH = levelH[std::clamp(trackLevel_, 0, 2)];
  std::vector<TrackId> vis;
  for (TrackId id : session_->trackIds()) if (!trackHidden(id)) vis.push_back(id);
  const double rate = session_->sampleRate();
  const float fpp = framesPerPixel_;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float timelineX = origin.x + headerW;
  const float timelineW = std::max(100.f, avail.x - headerW);
  viewWidthPx_ = timelineW;
  const bool areaHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  auto frameToX = [&](std::int64_t f) { return timelineX + static_cast<float>((static_cast<double>(f) - scrollFrames_) / fpp); };
  auto xToFrame = [&](float x) { return static_cast<std::int64_t>(scrollFrames_ + static_cast<double>(x - timelineX) * fpp); };
  const int rowsVisible = std::max(1, static_cast<int>((avail.y - rulerH) / laneH));
  firstTrack_ = std::clamp(firstTrack_, 0, std::max(0, static_cast<int>(vis.size()) - rowsVisible));
  ImGuiIO& io0 = ImGui::GetIO();
  if (areaHovered && io0.MouseWheel != 0.f && !io0.KeyCtrl && !io0.KeyShift) firstTrack_ = std::clamp(firstTrack_ - static_cast<int>(io0.MouseWheel > 0 ? 1 : -1), 0, std::max(0, static_cast<int>(vis.size()) - rowsVisible));

  // follow the playhead while playing
  if (engine_->transport().playing() && followPlayhead_) {
    const double p = static_cast<double>(position());
    if (p > scrollFrames_ + static_cast<double>(timelineW) * fpp * 0.95) scrollFrames_ = p - static_cast<double>(timelineW) * fpp * 0.05;
    if (p < scrollFrames_) scrollFrames_ = std::max(0.0, p);
  }

  // ----- tracks (drawn first so the rulers sit on top)
  float y = origin.y + rulerH;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float lanesTop = y;
  geo_ = TimelineGeometry{timelineX, lanesTop, laneH, static_cast<float>(fpp), scrollFrames_, firstTrack_};
  const float areaBottom = origin.y + avail.y;
  int rowIndex = 0;
  const int firstIdx = firstTrack_;
  for (std::size_t vi = static_cast<std::size_t>(firstIdx); vi < vis.size(); ++vi) {
    if (y + 20.f > areaBottom) break;
    const TrackId id = vis[vi];
    session::Track* t = session_->track(id);
    if (!t) continue;
    const bool selected = id == selTrack_;
    const ImU32 tc = st::trackColor(id);
    const float hh = std::min(laneH, areaBottom - y);
    // header
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    ImGui::PushID(id);
    ImGui::BeginChild("hdr", ImVec2(headerW - 4, hh - 2), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
      ImDrawList* hd = ImGui::GetWindowDrawList();
      const ImVec2 hp = ImGui::GetWindowPos();
      st::vgrad(hd, hp, ImVec2(hp.x + headerW - 4, hp.y + laneH - 2), selected ? col(0.27f, 0.29f, 0.33f) : col(0.21f, 0.22f, 0.245f), selected ? col(0.19f, 0.21f, 0.25f) : col(0.14f, 0.15f, 0.17f));
      st::vgrad(hd, hp, ImVec2(hp.x + 8, hp.y + laneH - 2), st::shade(tc, 1.15f), st::shade(tc, 0.7f));
      if (selected) hd->AddRect(hp, ImVec2(hp.x + headerW - 4, hp.y + laneH - 2), col(0.95f, 0.95f, 0.98f, 0.6f));
      const float W = headerW - 4.f;
      const bool small = trackLevel_ == 0;
      // name (white box, as a dark-on-light field)
      ImGui::SetCursorPos(ImVec2(14, 4));
      ImGui::SetNextItemWidth(small ? 112.f : W - 24.f);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, col(0.94f, 0.95f, 0.97f));
      ImGui::PushStyleColor(ImGuiCol_Text, col(0.05f, 0.06f, 0.08f));
      ImGui::InputText("##name", &t->name);
      ImGui::PopStyleColor(2);
      if (ImGui::IsItemActive() || ImGui::IsItemClicked()) selTrack_ = id;
      const bool armed = t->source->armed();
      const ImU32 recRed = 0xFF2F2FD8;
      auto sBtn = [&](const char* l, bool on, ImU32 c, float w) { return st::chip(l, on, c, ImVec2(w, 20.f)); };
      // buttons row
      ImGui::SetCursorPos(small ? ImVec2(132, 4) : ImVec2(14, 30));
      if (sBtn("R", armed, recRed, 22)) setArmGrouped(id, !armed);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Record enable");
      if (!small) {
        ImGui::SameLine(0, 2);
        ImGui::BeginDisabled(); sBtn("I", false, 0xFF3FA84A, 20); ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Input monitoring: not built yet");
      }
      ImGui::SameLine(0, 2);
      if (sBtn("S", t->solo, 0xFF1EB4E8, 22)) setSoloGrouped(id, !t->solo);
      ImGui::SameLine(0, 2);
      if (sBtn("M", t->strip->muted(), 0xFFD2882F, 22)) setMuteGrouped(id, !t->strip->muted());
      if (!small) {
        ImGui::SameLine(0, 6);
        if (sBtn("wave", showWaves_, 0xFF8A8C92, 36)) showWaves_ = !showWaves_;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show waveforms or plain blocks");
        ImGui::SameLine(0, 2);
        if (sBtn(!showVolAuto_ ? "auto" : autoLane_ == 0 ? "vol" : autoLane_ == 1 ? "pan" : "mute", showVolAuto_, 0xFF3FA84A, 34)) {   // off -> volume -> pan -> off
          if (!showVolAuto_) { showVolAuto_ = true; autoLane_ = 0; } else if (autoLane_ < 2) ++autoLane_; else showVolAuto_ = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automation on the timeline (click: volume, again: pan, again: mute, again: off). Click a lane to add a point, drag to move it, right-click to delete (while stopped)");
        ImGui::SameLine(0, 2);
        const bool au = t->strip->automationOn();
        if (sBtn(au ? "read" : "off", au, 0xFF3FA84A, 40)) t->strip->setAutomationOn(!au);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automation: read or off");
      }
      // I/O boxes and vol / pan
      const float rowY = small ? 30.f : 56.f;
      float db = t->strip->gainDb();
      if (!small) {
        ImGui::SetCursorPos(ImVec2(14, rowY));
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.55f, 0.57f, 0.62f));
        ImGui::TextUnformatted(t->mono ? "In 1" : "In 1-2");
        ImGui::PopStyleColor();
        const session::Bus* ob = session_->bus(t->output);
        ImGui::SetNextItemWidth(80);
        std::string outName = ob && t->output != session::kMaster ? ob->name : "Out 1-2";
        if (ImGui::BeginCombo("##out", outName.c_str())) {
          for (session::BusId bi : session_->busIds()) { const std::string n = bi == session::kMaster ? "Out 1-2" : session_->bus(bi)->name; if (ImGui::Selectable(n.c_str(), bi == t->output)) session_->setTrackOutput(id, bi); }
          ImGui::EndCombo();
        }
        ImGui::EndGroup();
        ImGui::SameLine(0, 8);
      } else ImGui::SetCursorPos(ImVec2(14, rowY));
      ImGui::BeginGroup();
      ImGui::SetNextItemWidth(small ? 80.f : 76.f);
      if (ImGui::DragFloat("##vol", &db, 0.1f, -60.f, 12.f, "vol %.1f")) { const float d = db; forGroup(id, [&](TrackId, session::Track* tt) { tt->strip->setGainDb(d); }); }
      if (ImGui::IsItemClicked() && ImGui::GetIO().KeyAlt) t->strip->setGainDb(0.f);
      ImGui::SameLine(0, 4);
      int pan = static_cast<int>(std::lround(t->strip->pan() * 100.f));
      ImGui::SetNextItemWidth(small ? 62.f : 56.f);
      if (ImGui::DragInt("##pan", &pan, 0.5f, -100, 100, "pan %d")) t->strip->setPan(static_cast<float>(pan) / 100.f);
      ImGui::EndGroup();
      if (trackLevel_ == 2) {   // large: a level strip along the bottom
        const auto it = levels_.find(t->strip);
        const float lv = it != levels_.end() ? std::max(it->second[0], it->second[1]) : 0.f;
        const float dbv = lv > 1e-5f ? 20.f * std::log10(lv) : -100.f;
        const float f = std::clamp((dbv + 60.f) / 66.f, 0.f, 1.f);
        const ImVec2 b0(hp.x + 14.f, hp.y + laneH - 20.f), b1(hp.x + W - 10.f, hp.y + laneH - 10.f);
        hd->AddRectFilled(b0, b1, col(0.03f, 0.03f, 0.04f), 2.f);
        hd->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * f, b1.y), dbv > -3.f ? col(0.95f, 0.25f, 0.2f) : dbv > -12.f ? col(0.95f, 0.8f, 0.2f) : col(0.2f, 0.9f, 0.4f), 2.f);
      }
      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) selTrack_ = id;
    }
    ImGui::EndChild();
    ImGui::PopID();

    // lane
    const ImVec2 a(timelineX, y), b(timelineX + timelineW, y + hh - 2);
    dl->AddRectFilled(a, b, (rowIndex & 1) ? col(0.085f, 0.09f, 0.10f) : col(0.100f, 0.105f, 0.115f));
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), col(0.f, 0.f, 0.f, 0.8f));
    if (mode_ == EditMode::Grid) {
      const double g = static_cast<double>(gridFrames());
      if (g / fpp > 8.0) {
        const double first = std::ceil(scrollFrames_ / g) * g;
        for (double f = first; f < scrollFrames_ + timelineW * fpp; f += g) {
          const float x = frameToX(static_cast<std::int64_t>(f));
          dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), col(1.f, 1.f, 1.f, 0.05f));
        }
      }
    }
    dl->PushClipRect(a, b, true);
    const auto playlist = t->source->playlist();
    for (const Clip& c : playlist) {
      const float x0 = frameToX(c.start), x1 = frameToX(c.start + c.length);
      if (x1 < a.x || x0 > b.x) continue;
      const bool csel = id == selTrack_ && c.id == selClip_;
      const ImVec2 c0(x0, a.y + 3), c1(x1, b.y - 3);
      st::vgrad(dl, c0, c1, st::shade(tc, csel ? 0.98f : 0.60f, 255), st::shade(tc, csel ? 0.74f : 0.38f, 255));
      dl->AddRectFilled(c0, ImVec2(c1.x, c0.y + 15.f), st::shade(tc, csel ? 1.12f : 0.76f, 255));
      dl->AddRect(c0, c1, csel ? col(1.f, 1.f, 1.f, 0.95f) : col(0.f, 0.f, 0.f, 0.85f));
      if (c.audio && showWaves_) {
        const PeakCache& pc = peaksFor(c.audio);
        const bool st2 = !pc.lo2.empty() && laneH > 100.f;
        const float top = c0.y + 16.f, areaH = c1.y - top - 2.f;
        const int lanes = st2 ? 2 : 1;
        const float xs = std::max(x0, a.x), xe = std::min(x1, b.x);
        const ImU32 ink = csel ? col(0.06f, 0.06f, 0.08f, 0.95f) : st::shade(tc, 1.5f, 235);
        for (int ln = 0; ln < lanes; ++ln) {
          const auto& L = ln == 0 ? pc.lo : pc.lo2; const auto& H = ln == 0 ? pc.hi : pc.hi2;
          const float mid = top + areaH / static_cast<float>(lanes) * (static_cast<float>(ln) + 0.5f), amp = areaH / static_cast<float>(lanes) * 0.5f * waveScale_;
          for (float x = xs; x < xe; x += 1.f) {
            const std::int64_t f0 = c.offset + static_cast<std::int64_t>((static_cast<double>(x - x0)) * fpp);
            const std::int64_t f1 = c.offset + static_cast<std::int64_t>((static_cast<double>(x + 1.f - x0)) * fpp);
            const std::size_t b0 = static_cast<std::size_t>(std::max<std::int64_t>(0, f0) / static_cast<std::int64_t>(PeakCache::kBucket));
            const std::size_t b1 = std::min(L.size(), static_cast<std::size_t>(std::max<std::int64_t>(f1, 0) / static_cast<std::int64_t>(PeakCache::kBucket)) + 1);
            float lo = 0.f, hi = 0.f;
            for (std::size_t k = b0; k < b1 && k < L.size(); ++k) { lo = std::min(lo, L[k]); hi = std::max(hi, H[k]); }
            dl->AddLine(ImVec2(x, mid - hi * amp), ImVec2(x, mid - lo * amp + 1.f), ink);
          }
        }
      }
      if (c.fadeIn > 0) dl->AddTriangleFilled(ImVec2(x0, c1.y), ImVec2(x0 + static_cast<float>(c.fadeIn) / fpp, c0.y), ImVec2(x0, c0.y), col(0.f, 0.f, 0.f, 0.35f));
      if (c.fadeOut > 0) dl->AddTriangleFilled(ImVec2(x1, c1.y), ImVec2(x1 - static_cast<float>(c.fadeOut) / fpp, c0.y), ImVec2(x1, c0.y), col(0.f, 0.f, 0.f, 0.35f));
      if (!c.gainEnv.points().empty()) {   // the clip gain line: +12 dB at the top of the lane, -36 dB at the bottom
        const auto& gp = c.gainEnv.points();
        auto gy = [&](float db) { return a.y + (12.f - std::clamp(db, -36.f, 12.f)) / 48.f * (b.y - a.y); };
        const ImU32 gcol = col(1.f, 0.85f, 0.25f, 0.95f);
        ImVec2 prev(x0, gy(static_cast<float>(gp.front().value)));
        for (const auto& q : gp) {
          const ImVec2 cur(frameToX(c.start + q.frame), gy(static_cast<float>(q.value)));
          dl->AddLine(prev, cur, gcol, 1.5f);
          dl->AddCircleFilled(cur, 2.5f, gcol);
          prev = cur;
        }
        dl->AddLine(prev, ImVec2(x1, prev.y), gcol, 1.5f);
      }
      if (x1 - x0 > 40.f) {
        const std::string nm = clipName(c, t->name);
        dl->PushClipRect(c0, ImVec2(c1.x, c0.y + 15.f), true);
        dl->AddText(ImVec2(std::max(x0, a.x) + 6, c0.y + 1), csel ? col(0.f, 0.f, 0.f, 0.9f) : col(1.f, 1.f, 1.f, 0.92f), nm.c_str());
        dl->PopClipRect();
        if (x1 - x0 > 70.f) {   // clip gain tag
          char g[16]; std::snprintf(g, sizeof g, "%.0f dB", static_cast<double>(c.gainDb));
          const ImVec2 gs = ImGui::CalcTextSize(g);
          const ImVec2 ga(std::max(x0, a.x) + 4.f, c1.y - gs.y - 5.f);
          dl->AddRectFilled(ga, ImVec2(ga.x + gs.x + 8.f, ga.y + gs.y + 3.f), col(0.f, 0.f, 0.f, 0.55f), 2.f);
          dl->AddText(ImVec2(ga.x + 4.f, ga.y + 1.f), col(1.f, 1.f, 1.f, 0.9f), g);
        }
      }
    }
    // crossfades where clips meet
    for (std::size_t ci = 0; ci + 1 < playlist.size(); ++ci) {
      const Clip& p0 = playlist[ci]; const Clip& p1 = playlist[ci + 1];
      const std::int64_t endP = p0.start + p0.length, tol = std::max<std::int64_t>({p0.fadeOut, p1.fadeIn, 64});
      if ((p0.fadeOut > 0 || p1.fadeIn > 0) && std::llabs(p1.start - endP) <= tol) {
        const float xj = frameToX((endP + p1.start) / 2), w = std::max(5.f, static_cast<float>(std::max(p0.fadeOut, p1.fadeIn)) / fpp * 0.5f);
        dl->AddLine(ImVec2(xj - w, b.y - 4), ImVec2(xj + w, a.y + 4), col(1.f, 1.f, 1.f, 0.9f), 1.5f);
        dl->AddLine(ImVec2(xj - w, a.y + 4), ImVec2(xj + w, b.y - 4), col(1.f, 1.f, 1.f, 0.9f), 1.5f);
      }
    }
    if (showVolAuto_) {   // the track's volume lane, drawn over its clips; editable while the song is stopped
      const int which = autoLane_;
      session::AutomationLane& vl = which == 0 ? t->strip->volumeLane() : which == 1 ? t->strip->panLane() : t->strip->muteLane();
      const float dbLo = which == 0 ? -60.f : which == 1 ? -1.f : 0.f, dbHi = which == 0 ? 12.f : 1.f, top = a.y + 6.f, bot = b.y - 6.f;
      auto Y = [&](float db) { return bot - (std::clamp(db, dbLo, dbHi) - dbLo) / (dbHi - dbLo) * (bot - top); };
      auto dbAt = [&](float yy) { return std::clamp(dbLo + (bot - yy) / (bot - top) * (dbHi - dbLo), dbLo, dbHi); };
      dl->AddRectFilled(a, b, col(0.f, 0.f, 0.f, 0.35f));
      dl->AddLine(ImVec2(a.x, Y(0.f)), ImVec2(b.x, Y(0.f)), col(1.f, 1.f, 1.f, 0.12f));
      const char* what = which == 0 ? "volume" : which == 1 ? "pan" : "mute";
      const bool stopped = !engine_->transport().playing();
      std::vector<session::AutomationPoint> pts = vl.points();
      const bool over = ImGui::IsWindowHovered() && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y && stopped;
      auto nearest = [&]() { int best = -1; float bd = 10.f; for (std::size_t i = 0; i < pts.size(); ++i) { const float d = std::hypot(frameToX(pts[i].frame) - mouse.x, Y(pts[i].value) - mouse.y); if (d < bd) { bd = d; best = static_cast<int>(i); } } return best; };
      const std::string key = std::string("voldraw:") + std::to_string(which) + ":" + std::to_string(id);
      const std::int64_t mf = std::max<std::int64_t>(0, snap(xToFrame(mouse.x)));
      if (over && ImGui::IsMouseClicked(0)) {
        selTrack_ = id; volDragTrack_ = id; volDrag_ = nearest();
        if (volDrag_ < 0) {
          auto it = std::lower_bound(pts.begin(), pts.end(), mf, [](const session::AutomationPoint& p0, std::int64_t f) { return p0.frame < f; });
          if (it == pts.end() || it->frame != mf) {
            volDrag_ = static_cast<int>(it - pts.begin());
            pts.insert(it, session::AutomationPoint{mf, dbAt(mouse.y)});
            session_->setStripLaneUndoable(undo_, id, which, pts, std::string("Draw ") + what + " automation");
          }
        }
      } else if (volDrag_ >= 0 && volDragTrack_ == id && ImGui::IsMouseDown(0) && volDrag_ < static_cast<int>(pts.size()) && stopped) {
        const std::size_t i = static_cast<std::size_t>(volDrag_);
        const std::int64_t lo = i > 0 ? pts[i - 1].frame + 1 : 0, hi = i + 1 < pts.size() ? pts[i + 1].frame - 1 : std::numeric_limits<std::int64_t>::max();
        const session::AutomationPoint np{std::clamp(mf, lo, std::max(lo, hi)), dbAt(mouse.y)};
        if (!(pts[i] == np)) { pts[i] = np; session_->setStripLaneUndoable(undo_, id, which, pts, std::string("Move ") + what + " automation point", key); }
      } else if (volDragTrack_ == id && !ImGui::IsMouseDown(0)) volDrag_ = -1;
      if (over && ImGui::IsMouseClicked(1)) {
        const int i = nearest();
        if (i >= 0) { pts.erase(pts.begin() + i); session_->setStripLaneUndoable(undo_, id, which, pts, std::string("Delete ") + what + " automation point"); }
      }
      const auto& now = vl.points();
      if (!now.empty()) {
        std::vector<ImVec2> pl;
        pl.push_back(ImVec2(a.x, Y(now.front().value)));
        for (const auto& pt : now) {
          if (which == 2) pl.push_back(ImVec2(frameToX(pt.frame), pl.back().y));   // mute is on/off: a step, not a slope
          pl.push_back(ImVec2(frameToX(pt.frame), Y(pt.value)));
        }
        pl.push_back(ImVec2(b.x, Y(now.back().value)));
        dl->AddPolyline(pl.data(), static_cast<int>(pl.size()), col(0.35f, 0.9f, 0.5f), 0, 2.f);
        for (const auto& pt : now) dl->AddCircleFilled(ImVec2(frameToX(pt.frame), Y(pt.value)), 3.5f, col(0.7f, 1.f, 0.78f));
      } else dl->AddText(ImVec2(a.x + 8.f, a.y + 4.f), col(0.8f, 0.8f, 0.85f, 0.7f), stopped ? (which == 0 ? "Click to draw volume automation" : which == 1 ? "Click to draw pan automation (up = right)" : "Click to draw mute automation (top = muted, bottom = playing)") : "Stop the song to edit automation");
    }
    dl->PopClipRect();
    y += laneH;
    ++rowIndex;
  }
  const float lanesBottom = std::min(y, areaBottom);
  // empty lane area under the last track
  if (y < areaBottom) dl->AddRectFilled(ImVec2(timelineX, y), ImVec2(timelineX + timelineW, areaBottom), col(0.075f, 0.078f, 0.088f));

  // ----- rulers (three rows)
  dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + headerW, origin.y + rulerH), col(0.13f, 0.135f, 0.15f));
  {
    const char* names[3] = {"Min:Secs", "Bars|Beats", "Markers"};
    for (int i = 0; i < 3; ++i) {
      dl->AddText(ImVec2(origin.x + 10.f, origin.y + rowRuler * static_cast<float>(i) + 2.f), col(0.62f, 0.65f, 0.70f), names[i]);
      dl->AddLine(ImVec2(origin.x, origin.y + rowRuler * static_cast<float>(i + 1)), ImVec2(timelineX + timelineW, origin.y + rowRuler * static_cast<float>(i + 1)), col(0.f, 0.f, 0.f));
    }
  }
  st::vgrad(dl, ImVec2(timelineX, origin.y), ImVec2(timelineX + timelineW, origin.y + rowRuler * 2.f), col(0.19f, 0.195f, 0.21f), col(0.12f, 0.125f, 0.135f));
  dl->AddRectFilled(ImVec2(timelineX, origin.y + rowRuler * 2.f), ImVec2(timelineX + timelineW, origin.y + rulerH), col(0.10f, 0.105f, 0.115f));
  for (int i = 0; i < 3; ++i) dl->AddLine(ImVec2(timelineX, origin.y + rowRuler * static_cast<float>(i + 1)), ImVec2(timelineX + timelineW, origin.y + rowRuler * static_cast<float>(i + 1)), col(0.f, 0.f, 0.f));
  dl->PushClipRect(ImVec2(timelineX, origin.y), ImVec2(timelineX + timelineW, origin.y + rulerH), true);
  {
    const double pxPerSec = rate / fpp;
    double step = 1.0;
    for (double s : {0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0}) if (s * pxPerSec >= 70.0) { step = s; break; }
    const double t0 = std::floor(scrollFrames_ / rate / step) * step;
    for (double t = t0; t * rate < scrollFrames_ + timelineW * fpp; t += step) {
      const float x = frameToX(static_cast<std::int64_t>(t * rate));
      dl->AddLine(ImVec2(x, origin.y + 8), ImVec2(x, origin.y + rowRuler), col(0.62f, 0.65f, 0.70f));
      for (int k = 1; k < 5; ++k) { const float mx = x + static_cast<float>(k) * static_cast<float>(step * pxPerSec) / 5.f; dl->AddLine(ImVec2(mx, origin.y + 13), ImVec2(mx, origin.y + rowRuler), col(0.45f, 0.48f, 0.53f)); }
      char b[24];
      const int mm = static_cast<int>(t / 60.0);
      if (step < 1.0) std::snprintf(b, sizeof b, "%d:%05.2f", mm, std::fmod(t, 60.0)); else std::snprintf(b, sizeof b, "%d:%02d", mm, static_cast<int>(std::fmod(t, 60.0)));
      dl->AddText(ImVec2(x + 3, origin.y + 1), col(0.78f, 0.80f, 0.84f), b);
    }
    // bars | beats
    const double beatF = rate * 60.0 / static_cast<double>(tempo_), barF = beatF * static_cast<double>(beatsPerBar_);
    double barStep = 1.0;
    for (double bs : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0}) if (bs * barF / fpp >= 46.0) { barStep = bs; break; }
    const double b0 = std::floor(scrollFrames_ / barF / barStep) * barStep;
    const float yb = origin.y + rowRuler;
    for (double bar = b0; bar * barF < scrollFrames_ + timelineW * fpp; bar += barStep) {
      const float x = frameToX(static_cast<std::int64_t>(bar * barF));
      dl->AddLine(ImVec2(x, yb + 4), ImVec2(x, yb + rowRuler), col(0.7f, 0.73f, 0.78f));
      char b[16]; std::snprintf(b, sizeof b, "%d", 1 + static_cast<int>(bar));
      dl->AddText(ImVec2(x + 3, yb + 1), col(0.8f, 0.82f, 0.86f), b);
      if (barStep == 1.0 && beatF / fpp > 9.0) for (int k = 1; k < 4; ++k) { const float bx = frameToX(static_cast<std::int64_t>(bar * barF + k * beatF)); dl->AddLine(ImVec2(bx, yb + 11), ImVec2(bx, yb + rowRuler), col(0.45f, 0.48f, 0.53f)); }
    }
    if (selEnd_ > selStart_) for (std::int64_t f : {selStart_, selEnd_}) {
      const float x = frameToX(f);
      dl->AddTriangleFilled(ImVec2(x - 4, yb + 2), ImVec2(x + 4, yb + 2), ImVec2(x, yb + 10), col(0.3f, 0.6f, 1.f));
    }
    // markers
    const float ym = origin.y + rowRuler * 2.f;
    for (std::size_t i = 0; i < markers_.size(); ++i) {
      const float x = frameToX(markers_[i].frame);
      if (x < timelineX - 120.f || x > timelineX + timelineW) continue;
      const ImU32 mc = i % 2 ? col(0.95f, 0.8f, 0.2f) : col(0.35f, 0.8f, 0.4f);
      dl->AddLine(ImVec2(x, ym), ImVec2(x, ym + rowRuler), mc, 1.5f);
      const ImVec2 ts = ImGui::CalcTextSize(markers_[i].name.c_str());
      dl->AddRectFilled(ImVec2(x, ym + 1), ImVec2(x + ts.x + 10.f, ym + rowRuler - 1), st::shade(mc, 0.5f, 255), 2.f);
      dl->AddText(ImVec2(x + 5.f, ym + 1), col(0.95f, 0.97f, 0.98f), markers_[i].name.c_str());
    }
  }
  dl->PopClipRect();

  // ----- edit cursor (green), playhead (red)
  {
    const float ex = frameToX(selStart_);
    if (ex >= timelineX && ex <= timelineX + timelineW) dl->AddLine(ImVec2(ex, origin.y + rowRuler * 2.f), ImVec2(ex, std::max(lanesBottom, origin.y + rulerH)), col(0.3f, 0.95f, 0.45f), 1.f);
    if (selEnd_ > selStart_) dl->AddRectFilled(ImVec2(std::max(timelineX, frameToX(selStart_)), lanesTop), ImVec2(std::min(timelineX + timelineW, frameToX(selEnd_)), lanesBottom), col(0.3f, 0.6f, 1.f, 0.18f));
    if (engine_->transport().looping()) dl->AddRectFilled(ImVec2(std::max(timelineX, frameToX(selStart_)), origin.y), ImVec2(std::min(timelineX + timelineW, frameToX(selEnd_)), origin.y + 4), col(0.9f, 0.7f, 0.1f));
    const float px = frameToX(position());
    if (px >= timelineX && px <= timelineX + timelineW) {
      dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + rulerH), col(1.f, 0.3f, 0.25f), 1.5f);
      if (engine_->transport().playing()) dl->AddLine(ImVec2(px, lanesTop), ImVec2(px, std::max(lanesBottom, lanesTop)), col(1.f, 0.3f, 0.25f, 0.8f), 1.5f);
      dl->AddTriangleFilled(ImVec2(px - 5, origin.y), ImVec2(px + 5, origin.y), ImVec2(px, origin.y + 8), col(1.f, 0.3f, 0.25f));
    }
  }

  // ----- mouse interaction over the timeline
  ImGui::SetCursorScreenPos(ImVec2(timelineX, origin.y));
  ImGui::InvisibleButton("##timeline", ImVec2(timelineW, std::max(rulerH + 10.f, lanesBottom - origin.y + 10.f)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.f && io.KeyCtrl) {   // zoom around the pointer
      const std::int64_t at = xToFrame(mouse.x);
      framesPerPixel_ = std::clamp(framesPerPixel_ * (io.MouseWheel > 0 ? 0.8f : 1.25f), 2.f, 200000.f);
      scrollFrames_ = std::max(0.0, static_cast<double>(at) - static_cast<double>(mouse.x - timelineX) * framesPerPixel_);
    } else if (io.MouseWheel != 0.f && io.KeyShift) scrollFrames_ = std::max(0.0, scrollFrames_ - static_cast<double>(io.MouseWheel) * timelineW * fpp * 0.1);
    if (io.MouseWheelH != 0.f) scrollFrames_ = std::max(0.0, scrollFrames_ - static_cast<double>(io.MouseWheelH) * timelineW * fpp * 0.1);
  }
  const int visRows = static_cast<int>(vis.size());
  auto rowAt = [&](float yy) -> int { if (yy < lanesTop) return -1; const int r = static_cast<int>((yy - lanesTop) / laneH) + firstIdx; return r < 0 || r >= visRows ? -1 : r; };
  // (a copy: the playlist is a temporary, a pointer into it would dangle)
  auto clipAt = [&](session::Track* t, std::int64_t f) -> std::optional<Clip> {
    std::optional<Clip> hit;
    for (const Clip& c : t->source->playlist()) if (f >= c.start && f < c.start + c.length) hit = c;
    return hit;
  };
  const float mkTop = origin.y + rowRuler * 2.f;
  if (hovered && ImGui::IsMouseDoubleClicked(0) && mouse.y >= mkTop && mouse.y < origin.y + rulerH) { addMarker(snap(xToFrame(mouse.x))); }
  else if (hovered && ImGui::IsMouseClicked(0) && !(showVolAuto_ && mouse.y >= lanesTop)) {
    const std::int64_t f = xToFrame(mouse.x);
    const int row = rowAt(mouse.y);
    const bool inRuler = mouse.y < lanesTop;
    dragKind_ = 0;
    if (inRuler) {
      bool onMarker = false;
      if (mouse.y >= mkTop) for (const Marker& m : markers_) if (std::fabs(mouse.x - frameToX(m.frame)) < 8.f) { engine_->transport().locate(m.frame); selStart_ = selEnd_ = m.frame; onMarker = true; break; }
      if (!onMarker) engine_->transport().locate(snap(f));
    } else if (row >= 0) {
      selTrack_ = vis[static_cast<std::size_t>(row)];
      session::Track* t = session_->track(selTrack_);
      const std::optional<Clip> c = clipAt(t, f);
      const bool lowerHalf = (mouse.y - lanesTop) - static_cast<float>(row - firstIdx) * laneH > laneH * 0.5f;
      Tool tool = tool_;
      if (tool == Tool::Smart) tool = lowerHalf ? Tool::Grabber : Tool::Selector;
      selClip_ = c ? c->id : -1;
      if (tool == Tool::Zoomer) {
        framesPerPixel_ = std::clamp(ImGui::GetIO().KeyAlt ? framesPerPixel_ * 2.f : framesPerPixel_ * 0.5f, 2.f, 200000.f);
        scrollFrames_ = std::max(0.0, static_cast<double>(f) - static_cast<double>(mouse.x - timelineX) * framesPerPixel_);
      } else if (tool == Tool::Pencil && c) {   // draw the clip's gain line freehand: top of the lane +12 dB, bottom -36 dB
        dragKind_ = 6; dragging_ = true; pencilLast_ = -1;
        dragLaneTop_ = lanesTop + static_cast<float>(row - firstIdx) * laneH; dragLaneH_ = laneH;
      } else if (tool == Tool::Scrubber) {   // drag to hear the audio under the pointer
        dragKind_ = 5; dragging_ = true;
        engine_->transport().locate(f);
        if (!engine_->transport().playing()) engine_->transport().play();
      } else if (c && (tool == Tool::Grabber || tool == Tool::Trimmer)) {
        dragOrigin_ = f; dragClipStart_ = c->start; dragClipOffset_ = c->offset; dragClipLength_ = c->length;
        const float x0 = frameToX(c->start), x1 = frameToX(c->start + c->length);
        if (tool == Tool::Trimmer) dragKind_ = std::fabs(mouse.x - x0) < std::fabs(mouse.x - x1) ? 2 : 3;
        else dragKind_ = (mouse.x - x0 < 6.f) ? 2 : (x1 - mouse.x < 6.f) ? 3 : 1;
        dragging_ = true;
      } else {
        dragKind_ = 4; dragOrigin_ = snap(f); selStart_ = selEnd_ = dragOrigin_; dragging_ = true;
        engine_->transport().locate(dragOrigin_);
      }
    }
  }
  if (dragging_) {
    if (!ImGui::IsMouseDown(0)) { if (dragKind_ == 5) engine_->transport().stop(); dragging_ = false; dragKind_ = 0; }
    else if (dragKind_ == 5) engine_->transport().locate(xToFrame(mouse.x));
    else if (dragKind_ == 6) {
      session::Track* t = session_->track(selTrack_);
      if (t) {
        auto list = t->source->playlist();
        const std::int64_t f = xToFrame(mouse.x);
        for (Clip& c : list) if (c.id == selClip_ && f >= c.start && f < c.start + c.length) {
          const float u = std::clamp((mouse.y - dragLaneTop_) / std::max(1.f, dragLaneH_), 0.f, 1.f);
          const float db = 12.f - u * 48.f;
          const std::int64_t rel = f - c.start;
          if (pencilLast_ < 0 || std::llabs(rel - pencilLast_) >= static_cast<std::int64_t>(framesPerPixel_ * 3.f)) {
            c.gainEnv.add(rel, db);
            pencilLast_ = rel;
            t->source->setPlaylist(std::move(list));
          }
          break;
        }
      }
    }
    else if (dragKind_ == 4) {
      const std::int64_t f = snap(xToFrame(mouse.x));
      selStart_ = std::min(dragOrigin_, f); selEnd_ = std::max(dragOrigin_, f);
    } else if (dragKind_ >= 1 && dragKind_ <= 3) {
      session::Track* t = session_->track(selTrack_);
      if (t) {
        auto list = t->source->playlist();
        for (Clip& c : list) if (c.id == selClip_) {
          const std::int64_t d = xToFrame(mouse.x) - dragOrigin_;
          if (dragKind_ == 1) c.start = snap(dragClipStart_ + d);
          else if (dragKind_ == 2) {   // trim start: the audio stays where it is on the timeline
            std::int64_t ns = std::clamp<std::int64_t>(snap(dragClipStart_ + d), dragClipStart_ - dragClipOffset_, dragClipStart_ + dragClipLength_ - 64);
            c.start = ns; c.offset = dragClipOffset_ + (ns - dragClipStart_); c.length = dragClipLength_ - (ns - dragClipStart_);
          } else {
            const std::int64_t room = c.audio ? static_cast<std::int64_t>(c.audio->frames) - c.offset : dragClipLength_;
            c.length = std::clamp<std::int64_t>(snap(dragClipStart_ + dragClipLength_ + d) - c.start, 64, room);
          }
        }
        t->source->setPlaylist(std::move(list));
      }
    }
  }
  if (hovered && ImGui::IsMouseClicked(1)) {
    if (mouse.y >= mkTop && mouse.y < lanesTop) {
      ImGui::OpenPopup("##marker_menu");
    } else ImGui::OpenPopup("##lane_menu");
  }
  if (ImGui::BeginPopup("##marker_menu")) {
    const std::int64_t f = xToFrame(mouse.x);
    if (ImGui::MenuItem("Add marker here")) addMarker(snap(f));
    for (std::size_t i = 0; i < markers_.size(); ++i) if (std::fabs(frameToX(markers_[i].frame) - mouse.x) < 40.f) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::SetNextItemWidth(140); ImGui::InputText("##mn", &markers_[i].name);
      if (ImGui::MenuItem("Delete marker")) { markers_.erase(markers_.begin() + static_cast<std::ptrdiff_t>(i)); ImGui::PopID(); break; }
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##lane_menu")) {
    if (ImGui::MenuItem("Separate clip", nullptr, false, true)) execute(Command::SeparateClip);
    if (ImGui::MenuItem("Duplicate clip")) execute(Command::Duplicate);
    if (ImGui::MenuItem("Create fades")) execute(Command::CreateFades);
    if (ImGui::MenuItem("Delete clip")) execute(Command::DeleteSelection);
    ImGui::EndPopup();
  }
  if (vis.empty()) { ImGui::SetCursorScreenPos(ImVec2(origin.x + 20, origin.y + rulerH + 30)); ImGui::TextDisabled("No tracks. Press + to add one."); }
}

}  // namespace wezt::ui
