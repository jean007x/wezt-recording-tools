// "Plugin Parameters": every parameter of an insert as a slider, with automation. Turn on "Write
// automation", press play and move a control: the movement is recorded as an automation lane that
// plays back with the song. A green dot marks parameters that have a lane; the graph shows it.
#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "ui/app.h"

namespace wezt::ui {

void App::drawPluginParamsWindow() {
  ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Plugin Parameters", &showPlugParams_)) { ImGui::End(); return; }
  const auto ids = session_->trackIds();
  if (ids.empty()) { ImGui::TextDisabled("No tracks yet."); ImGui::End(); return; }
  if (ppTrack_ < 0 || !session_->track(ppTrack_)) ppTrack_ = ids.front();
  session::Track* tr = session_->track(ppTrack_);
  ImGui::SetNextItemWidth(180);
  if (ImGui::BeginCombo("Track", tr->name.c_str())) {
    for (auto id : ids) if (ImGui::Selectable((session_->track(id)->name + "##pp" + std::to_string(id)).c_str(), id == ppTrack_)) { ppTrack_ = id; ppInsert_ = 0; ppParam_ = -1; }
    ImGui::EndCombo();
  }
  tr = session_->track(ppTrack_);
  if (tr->inserts.empty()) { ImGui::TextDisabled("This track has no plugins. Add one from the mixer, or ask the assistant: \"add a compressor to %s\".", tr->name.c_str()); ImGui::End(); return; }
  ppInsert_ = std::clamp(ppInsert_, 0, static_cast<int>(tr->inserts.size()) - 1);
  auto nameOf = [](const session::InsertSlot& s) { return s.instance && !s.instance->displayName().empty() ? s.instance->displayName() : s.descriptor.name; };
  ImGui::SameLine(); ImGui::SetNextItemWidth(200);
  if (ImGui::BeginCombo("Plugin", nameOf(tr->inserts[static_cast<std::size_t>(ppInsert_)]).c_str())) {
    for (std::size_t i = 0; i < tr->inserts.size(); ++i) if (ImGui::Selectable((nameOf(tr->inserts[i]) + "##ppi" + std::to_string(i)).c_str(), static_cast<int>(i) == ppInsert_)) { ppInsert_ = static_cast<int>(i); ppParam_ = -1; }
    ImGui::EndCombo();
  }
  auto& slot = tr->inserts[static_cast<std::size_t>(ppInsert_)];
  if (!slot.instance) { ImGui::TextDisabled("This plugin is not available (offline), so its parameters cannot be shown."); ImGui::End(); return; }
  const bool canAuto = slot.instance->supportsAutomation();
  const bool playing = engine_->transport().playing();
  ImGui::Checkbox("Write automation", &ppWrite_);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("While the song plays, moving a control records its movement as automation.");
  if (!canAuto) { ImGui::SameLine(); ImGui::TextDisabled("(this plugin cannot be automated)"); ppWrite_ = false; }
  if (ppWrite_ && playing) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.35f, 0.3f, 1), "writing"); }
  // when the song stops, tidy what was recorded: drop points that lie on a straight line
  if (ppWasPlaying_ && !playing) {
    const auto lanes = slot.paramLanes;   // a copy: setting a lane replaces entries of slot.paramLanes while we loop
    for (const auto& l : lanes) {
      session::AutomationLane tmp; for (const auto& p : l.lane.points()) tmp.add(p.frame, p.value);
      const std::size_t before = tmp.points().size(); tmp.thin(0.004f);
      if (tmp.points().size() != before) session_->setParamAutomationUndoable(undo_, ppTrack_, static_cast<std::size_t>(ppInsert_), l.id, tmp.points(), "Record plug-in automation", "plugauto:" + std::to_string(ppTrack_) + ":" + std::to_string(ppInsert_) + ":" + std::to_string(l.id));
    }
  }
  ppWasPlaying_ = playing;
  ImGui::Separator();

  const auto params = slot.instance->parameters();
  const std::int64_t pos = engine_->transport().position();
  ImGui::BeginChild("params", ImVec2(0, -150), false);
  std::size_t shown = 0;
  for (const auto& p : params) {
    if (++shown > 200) { ImGui::TextDisabled("... and %zu more", params.size() - 200); break; }
    ImGui::PushID(static_cast<int>(p.id));
    const auto* lane = session_->paramAutomation(ppTrack_, static_cast<std::size_t>(ppInsert_), p.id);
    const bool has = lane != nullptr;
    if (ImGui::SmallButton(has ? "\xE2\x97\x8F" : "\xE2\x97\x8B")) ppParam_ = static_cast<int>(p.id);   // select for the graph
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(has ? "Has automation. Click to show it below." : "No automation. Click to select.");
    ImGui::SameLine();
    float v = static_cast<float>(slot.instance->parameterNormalized(p.id));
    ImGui::SetNextItemWidth(220);
    const std::string text = slot.instance->parameterText(p.id, v);
    if (ImGui::SliderFloat(p.name.c_str(), &v, 0.f, 1.f, text.c_str())) {
      slot.instance->setParameterNormalized(p.id, v);
      ppParam_ = static_cast<int>(p.id);
      if (ppWrite_ && playing && canAuto) {   // record the movement at the playhead
        session::AutomationLane tmp; if (lane) for (const auto& pt : lane->points()) tmp.add(pt.frame, pt.value);
        tmp.add(pos, v);
        session_->setParamAutomationUndoable(undo_, ppTrack_, static_cast<std::size_t>(ppInsert_), p.id, tmp.points(), "Record plug-in automation", "plugauto:" + std::to_string(ppTrack_) + ":" + std::to_string(ppInsert_) + ":" + std::to_string(p.id));
      }
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  // graph of the selected parameter's lane
  ImGui::Separator();
  const plugins::ParamInfo* sel = nullptr;
  for (const auto& p : params) if (static_cast<int>(p.id) == ppParam_) sel = &p;
  if (!sel) { ImGui::TextDisabled("Click a dot to see that parameter's automation."); ImGui::End(); return; }
  const auto* lane = session_->paramAutomation(ppTrack_, static_cast<std::size_t>(ppInsert_), sel->id);
  ImGui::Text("%s", sel->name.c_str());
  if (lane) { ImGui::SameLine(); if (ImGui::SmallButton("Clear automation")) { session_->setParamAutomationUndoable(undo_, ppTrack_, static_cast<std::size_t>(ppInsert_), sel->id, {}, "Clear plug-in automation"); lane = nullptr; } }
  const ImVec2 p0 = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x, h = 90.f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(24, 26, 30, 255));
  const double rate = session_->sampleRate();
  const std::int64_t end = std::max<std::int64_t>(lane && !lane->points().empty() ? lane->points().back().frame : 0, static_cast<std::int64_t>(rate * 10.0));
  auto X = [&](std::int64_t f) { return p0.x + static_cast<float>(static_cast<double>(f) / static_cast<double>(end)) * w; };
  auto Y = [&](float n) { return p0.y + h - 4 - n * (h - 8); };
  // drawing: click an empty spot to add a point, drag a point to move it, right-click a point to delete it
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##autograph", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  if (canAuto) {
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mp = ImGui::GetIO().MousePos;
    std::vector<session::AutomationPoint> pts; if (lane) pts = lane->points();
    const std::string key = "plugdraw:" + std::to_string(ppTrack_) + ":" + std::to_string(ppInsert_) + ":" + std::to_string(sel->id);
    auto commit = [&](const std::vector<session::AutomationPoint>& v, const char* label, const std::string& mergeKey) {
      session_->setParamAutomationUndoable(undo_, ppTrack_, static_cast<std::size_t>(ppInsert_), sel->id, v, label, mergeKey);
    };
    auto nearest = [&]() { int best = -1; float bd = 10.f; for (std::size_t i = 0; i < pts.size(); ++i) { const float d = std::hypot(X(pts[i].frame) - mp.x, Y(pts[i].value) - mp.y); if (d < bd) { bd = d; best = static_cast<int>(i); } } return best; };
    const std::int64_t mf = static_cast<std::int64_t>(std::clamp((mp.x - p0.x) / w, 0.f, 1.f) * static_cast<float>(end));
    const float mv = std::clamp(1.f - (mp.y - p0.y - 4.f) / (h - 8.f), 0.f, 1.f);
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      ppDrag_ = nearest();
      if (ppDrag_ < 0) {
        auto it = std::lower_bound(pts.begin(), pts.end(), mf, [](const session::AutomationPoint& a, std::int64_t f) { return a.frame < f; });
        if (it != pts.end() && it->frame == mf) { ppDrag_ = -1; }   // a point already sits on this frame: no second one
        else {
          ppDrag_ = static_cast<int>(it - pts.begin());
          pts.insert(it, session::AutomationPoint{mf, mv});
          commit(pts, "Draw plug-in automation", "");
        }
      }
    } else if (ppDrag_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ppDrag_ < static_cast<int>(pts.size())) {
      const std::size_t i = static_cast<std::size_t>(ppDrag_);
      const std::int64_t lo = i > 0 ? pts[i - 1].frame + 1 : 0, hi = i + 1 < pts.size() ? pts[i + 1].frame - 1 : end;
      const session::AutomationPoint np{std::clamp(mf, lo, std::max(lo, hi)), mv};
      if (!(pts[i] == np)) { pts[i] = np; commit(pts, "Move plug-in automation point", key); }
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      ppDrag_ = -1;
    }
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      const int i = nearest();
      if (i >= 0) { pts.erase(pts.begin() + i); commit(pts, "Delete plug-in automation point", ""); }
    }
    lane = session_->paramAutomation(ppTrack_, static_cast<std::size_t>(ppInsert_), sel->id);   // the lane was replaced by an edit
  }
  if (lane && !lane->points().empty()) {
    std::vector<ImVec2> pl;
    pl.push_back(ImVec2(p0.x, Y(lane->points().front().value)));
    for (const auto& pt : lane->points()) pl.push_back(ImVec2(X(pt.frame), Y(pt.value)));
    pl.push_back(ImVec2(p0.x + w, Y(lane->points().back().value)));
    dl->AddPolyline(pl.data(), static_cast<int>(pl.size()), IM_COL32(80, 200, 110, 255), 0, 2.f);
    for (const auto& pt : lane->points()) dl->AddCircleFilled(ImVec2(X(pt.frame), Y(pt.value)), 3.f, IM_COL32(150, 240, 170, 255));
    if (pos <= end) dl->AddLine(ImVec2(X(pos), p0.y), ImVec2(X(pos), p0.y + h), IM_COL32(255, 90, 80, 200));
  } else {
    dl->AddText(ImVec2(p0.x + 8, p0.y + h / 2 - 8), IM_COL32(140, 140, 150, 255), canAuto ? "No automation. Click here to draw points, or turn on \"Write automation\" and move the control." : "This plug-in does not support automation.");
  }
  ImGui::End();
}

}  // namespace wezt::ui
