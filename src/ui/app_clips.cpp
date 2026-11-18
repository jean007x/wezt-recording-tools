// Clips window: every clip on every track in one list, and the playlists (alternate takes) of the
// selected track.
#include <algorithm>
#include <cstdio>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "edit/clip_tools.h"
#include "ui/app.h"

namespace wezt::ui {

using session::Clip;
using session::TrackId;

std::vector<App::ClipRow> App::clipRows() const {
  std::vector<ClipRow> rows;
  for (TrackId t : session_->trackIds()) {
    const session::Track* tr = session_->track(t);
    for (const Clip& c : tr->source->playlist()) rows.push_back({t, c.id, tr->name, c.name.empty() ? "(unnamed)" : c.name, c.start, c.length, c.gainDb, c.muted, c.locked});
  }
  std::stable_sort(rows.begin(), rows.end(), [](const ClipRow& a, const ClipRow& b) { return a.start < b.start; });
  return rows;
}

namespace {
bool edited(session::Session& s, TrackId t, int clip, const std::function<bool(Clip&)>& f) {
  session::Track* tr = s.track(t);
  if (!tr) return false;
  auto pl = tr->source->playlist();
  for (Clip& c : pl) if (c.id == clip) { if (!f(c)) return false; tr->source->setPlaylist(std::move(pl)); return true; }
  return false;
}
}  // namespace

bool App::renameClip(TrackId t, int clip, const std::string& name) { return edited(*session_, t, clip, [&](Clip& c) { c.name = name; return true; }); }
bool App::muteClip(TrackId t, int clip, bool on) { return edited(*session_, t, clip, [&](Clip& c) { c.muted = on; return true; }); }
bool App::setClipGainDb(TrackId t, int clip, float db) { return edited(*session_, t, clip, [&](Clip& c) { if (c.locked) return false; c.gainDb = std::clamp(db, -60.f, 24.f); return true; }); }
bool App::deleteClip(TrackId t, int clip) {
  session::Track* tr = session_->track(t);
  if (!tr) return false;
  for (const Clip& c : tr->source->playlist()) if (c.id == clip && c.locked) return false;
  const bool ok = session_->removeClip(t, clip);
  if (ok && selClip_ == clip && selTrack_ == t) selClip_ = -1;
  return ok;
}
bool App::locateClip(TrackId t, int clip) {
  session::Track* tr = session_->track(t);
  if (!tr) return false;
  for (const Clip& c : tr->source->playlist()) if (c.id == clip) {
    selectClip(t, clip);
    engine_->transport().locate(c.start);
    setRange(c.start, c.start + c.length);
    return true;
  }
  return false;
}

void App::drawClipsWindow() {
  ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Clips and Playlists", &showClips_)) { ImGui::End(); return; }
  const double sr = session_->sampleRate();
  auto secs = [&](std::int64_t f) { char b[32]; const double s = static_cast<double>(f) / sr; std::snprintf(b, sizeof b, "%d:%05.2f", static_cast<int>(s / 60), s - 60.0 * static_cast<int>(s / 60)); return std::string(b); };

  // ---- playlists of the selected track ----
  session::Track* sel = session_->track(selTrack_);
  if (sel) {
    ImGui::Text("Playlists of \"%s\":", sel->name.c_str());
    const auto names = session_->playlistNames(selTrack_);
    const std::string live = session_->livePlaylist(selTrack_);
    for (const auto& n : names) {
      ImGui::SameLine();
      const bool isLive = n == live;
      if (isLive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.5f, 0.85f, 1.f));
      if (ImGui::SmallButton((n + "##pl").c_str()) && !isLive) session_->switchPlaylist(selTrack_, n);
      if (isLive) ImGui::PopStyleColor();
    }
    if (ImGui::SmallButton("New (copy)")) { std::string n = "Take " + std::to_string(names.size() + 1); while (!session_->newPlaylist(selTrack_, n, true)) n += "'"; }
    ImGui::SameLine();
    if (ImGui::SmallButton("New (empty)")) { std::string n = "Take " + std::to_string(names.size() + 1); while (!session_->newPlaylist(selTrack_, n, false)) n += "'"; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("##plname", &playlistInput_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Rename live") && !playlistInput_.empty() && session_->renamePlaylist(selTrack_, live, playlistInput_)) playlistInput_.clear();
    ImGui::SameLine();
    const bool haveSel = selEnd_ > selStart_;
    ImGui::TextDisabled(haveSel ? "Comp the selection from:" : "Select a range to comp");
    if (haveSel) for (const auto& n : names) if (n != live) { ImGui::SameLine(); if (ImGui::SmallButton(("<- " + n + "##comp").c_str())) session_->compFromPlaylist(selTrack_, n, selStart_, selEnd_); }
    for (const auto& n : names) if (n != live) { ImGui::SameLine(); if (ImGui::SmallButton(("x " + n + "##del").c_str())) session_->deletePlaylist(selTrack_, n); }
    ImGui::Separator();
  } else ImGui::TextDisabled("Select a track to see its playlists.");

  // ---- all clips ----
  const auto rows = clipRows();
  ImGui::Text("%zu clips", rows.size());
  if (ImGui::BeginTable("clips", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupColumn("Track"); ImGui::TableSetupColumn("Clip"); ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 70.f);
    ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 70.f); ImGui::TableSetupColumn("Gain dB", ImGuiTableColumnFlags_WidthFixed, 80.f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.f); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 56.f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    int delT = -1, delC = -1;
    for (const ClipRow& r : rows) {
      ImGui::PushID(r.track * 100003 + r.clip);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const bool selected = r.track == selTrack_ && r.clip == selClip_;
      if (ImGui::Selectable(r.trackName.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) locateClip(r.track, r.clip);
      ImGui::TableNextColumn();
      std::string nm = r.name == "(unnamed)" ? "" : r.name;
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputTextWithHint("##n", "(unnamed)", &nm, ImGuiInputTextFlags_EnterReturnsTrue)) renameClip(r.track, r.clip, nm);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(secs(r.start).c_str());
      ImGui::TableNextColumn(); ImGui::TextUnformatted(secs(r.length).c_str());
      ImGui::TableNextColumn();
      float g = r.gainDb; ImGui::SetNextItemWidth(-1);
      if (ImGui::DragFloat("##g", &g, 0.1f, -60.f, 24.f, "%.1f")) setClipGainDb(r.track, r.clip, g);
      ImGui::TableNextColumn();
      bool m = r.muted; if (ImGui::Checkbox("mute", &m)) muteClip(r.track, r.clip, m);
      if (r.locked) { ImGui::SameLine(); ImGui::TextDisabled("L"); }
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Delete") && !r.locked) { delT = r.track; delC = r.clip; }
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (delT >= 0) deleteClip(delT, delC);
  }
  ImGui::End();
}

}  // namespace wezt::ui
