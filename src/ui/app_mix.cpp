// Mix window: channel strips with inserts, sends, pan, fader, meters.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "ui/app.h"
#include "ui/style.h"

namespace wezt::ui {
using session::BusId;
using session::TrackId;

namespace {
ImU32 col(float r, float g, float b, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }
bool toggle(const char* label, bool on, ImVec4 c, ImVec2 size) {
  if (on) { ImGui::PushStyleColor(ImGuiCol_Button, c); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c); }
  const bool p = ImGui::Button(label, size);
  if (on) ImGui::PopStyleColor(2);
  return p;
}
}  // namespace

void App::drawMix() {
  const float stripW = 112.f;
  const auto ids = session_->trackIds();
  const auto busIds = session_->busIds();
  ImGui::BeginChild("##mixscroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  auto drawStrip = [&](const char* title, session::Strip* strip, session::Track* track, TrackId tid, BusId bid, bool isMaster) {
    const bool isBus = !track;
    ImGui::PushID(isMaster ? 9999 : isBus ? 5000 + bid : tid);
    ImGui::BeginChild("strip", ImVec2(stripW, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    const ImU32 sc = track ? style::trackColor(tid) : isMaster ? 0xFF3A3AE0 : 0xFFA8A8A0;
    {
      ImDrawList* d = ImGui::GetWindowDrawList();
      const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
      style::vgrad(d, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), col(0.19f, 0.195f, 0.21f), col(0.125f, 0.13f, 0.14f));
      style::vgrad(d, wp, ImVec2(wp.x + ws.x, wp.y + 12.f), style::shade(sc, 1.15f), style::shade(sc, 0.7f));
    }
    ImGui::Dummy(ImVec2(0, 8));
    const bool selected = track && tid == selTrack_;
    if (selected) ImGui::GetWindowDrawList()->AddRect(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + stripW, ImGui::GetWindowPos().y + ImGui::GetWindowHeight()), col(0.95f, 0.95f, 0.98f, 0.6f));
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && track) selTrack_ = tid;
    // inserts and sends live in a fixed-height area so every fader lines up
    ImGui::BeginChild("##routing", ImVec2(0, 172), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled("Inserts");
    if (track) {
      for (std::size_t i = 0; i < track->inserts.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto& in = track->inserts[i];
        const bool offline = !in.instance;
        if (offline) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.5f, 0.3f, 1.f));
        if (ImGui::Selectable(in.descriptor.name.c_str(), false, 0, ImVec2(stripW - 36, 0))) {
          if (dynamic_cast<instruments::Sampler*>(in.instance)) { selTrack_ = tid; showSampler_ = true; }
        }
        if (offline) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { const bool was = deviceRunning_; if (was) device_->stop(); session_->removeInsert(tid, i); if (was) { std::string e; device_->start(&e); } ImGui::PopID(); break; }
        ImGui::PopID();
      }
      if (ImGui::SmallButton("+ Sampler")) {
        const bool was = deviceRunning_; if (was) device_->stop();
        session_->addInsert(tid, std::make_unique<instruments::Sampler>(), instruments::Sampler::descriptor(), 0);
        if (was) { std::string e; device_->start(&e); }
        selTrack_ = tid; showSampler_ = true;
      }
    } else ImGui::TextDisabled("-");
    ImGui::Separator();
    // sends
    ImGui::TextDisabled("Sends");
    if (track) {
      for (std::size_t i = 0; i < track->sends.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 100);
        auto& s = track->sends[i];
        const session::Bus* b = session_->bus(s.bus);
        ImGui::Text("%s", b ? b->name.c_str() : "?");
        float lv = s.levelDb;
        ImGui::SetNextItemWidth(stripW - 16);
        if (ImGui::SliderFloat("##lv", &lv, -60.f, 6.f, "%.0f dB")) session_->setSendLevel(tid, i, lv);
        ImGui::PopID();
      }
      if (!busIds.empty()) {
        if (ImGui::SmallButton("+ Send")) ImGui::OpenPopup("sendmenu");
        if (ImGui::BeginPopup("sendmenu")) {
          for (BusId b : busIds) if (b != session::kMaster && ImGui::MenuItem(session_->bus(b)->name.c_str())) {
            const bool was = deviceRunning_; if (was) device_->stop();
            session_->addSend(tid, b, -6.f, false);
            if (was) { std::string e; device_->start(&e); }
          }
          ImGui::EndPopup();
        }
      }
    }
    ImGui::EndChild();
    // input / output boxes and automation mode
    if (track) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.57f, 0.62f, 1.f));
      ImGui::TextUnformatted(track->mono ? "In 1" : "In 1-2");
      ImGui::PopStyleColor();
      const session::Bus* ob = session_->bus(track->output);
      ImGui::SetNextItemWidth(stripW - 16);
      if (ImGui::BeginCombo("##out", ob && track->output != session::kMaster ? ob->name.c_str() : "Out 1-2")) {
        for (BusId b : busIds) { const std::string n = b == session::kMaster ? "Out 1-2" : session_->bus(b)->name; if (ImGui::Selectable(n.c_str(), b == track->output)) session_->setTrackOutput(tid, b); }
        ImGui::EndCombo();
      }
    }
    {
      const bool au = strip->automationOn();
      if (style::chip(au ? "auto: read" : "auto: off", au, 0xFF3FA84A, ImVec2(stripW - 16, 20))) strip->setAutomationOn(!au);
    }
    // pan knob
    {
      float pan = strip->pan();
      ImGui::SetCursorPosX((stripW - 40.f) * 0.5f);
      if (style::knob("##pan", "pan", &pan, -1.f, 1.f, 0.f, "%+.2f", 0xFFE8B84A, 30.f, true)) strip->setPan(pan);
    }
    // mute / solo / rec
    if (toggle("M", strip->muted(), ImVec4(0.2f, 0.45f, 0.7f, 1.f), ImVec2(30, 22))) { if (track) setMuteGrouped(tid, !strip->muted()); else strip->setMute(!strip->muted()); }
    if (track) {
      ImGui::SameLine();
      if (toggle("S", track->solo, ImVec4(0.8f, 0.7f, 0.1f, 1.f), ImVec2(30, 22))) setSoloGrouped(tid, !track->solo);
      ImGui::SameLine();
      const bool armed = track->source->armed();
      if (toggle("R", armed, ImVec4(0.8f, 0.1f, 0.1f, 1.f), ImVec2(30, 22))) setArmGrouped(tid, !armed);
    }
    // fader + meter
    float db = strip->gainDb();
    const float avail = std::max(120.f, ImGui::GetContentRegionAvail().y - 70.f);
    ImGui::SetCursorPosX(10);
    if (style::fader("##fader", &db, ImVec2(40, avail))) strip->setGainDb(db);
    const ImVec2 fp = ImGui::GetItemRectMin();
    ImGui::SameLine();
    const ImVec2 mp = ImGui::GetCursorScreenPos();
    const float pl = levels_[strip][0], pr = levels_[strip][1];
    style::ledMeter(ImGui::GetWindowDrawList(), ImVec2(mp.x + 2, fp.y + 2), ImVec2(9, avail - 4), pl, strip->meter().clipped());
    style::ledMeter(ImGui::GetWindowDrawList(), ImVec2(mp.x + 13, fp.y + 2), ImVec2(9, avail - 4), pr, strip->meter().clipped());
    ImGui::Dummy(ImVec2(26, avail));
    if (ImGui::IsItemClicked()) strip->meter().resetClip();
    ImGui::TextDisabled("%.1f dB", static_cast<double>(db));
    // name
    if (track) {
      ImGui::SetNextItemWidth(stripW - 16);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, style::shade(sc, 0.55f, 255));
      ImGui::InputText("##nm", &track->name);
      ImGui::PopStyleColor();
    } else ImGui::TextUnformatted(title);
    ImGui::EndChild();
    ImGui::PopID();
    ImGui::SameLine();
  };
  for (TrackId id : ids) { session::Track* t = session_->track(id); drawStrip(t->name.c_str(), t->strip, t, id, 0, false); }
  for (BusId b : busIds) {
    if (b == session::kMaster) continue;
    session::Bus* bus = session_->bus(b);
    drawStrip(bus->name.c_str(), bus->strip, nullptr, -1, b, false);
  }
  if (session::Bus* m = session_->bus(session::kMaster)) drawStrip("Master", m->strip, nullptr, -1, session::kMaster, true);
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (ImGui::Button("+ Aux bus")) {
    const bool was = deviceRunning_; if (was) device_->stop();
    session_->addBus("Aux " + std::to_string(busIds.size()));
    if (was) { std::string e; device_->start(&e); }
  }
  ImGui::EndGroup();
  ImGui::EndChild();
}

}  // namespace wezt::ui
