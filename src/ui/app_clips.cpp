// Clips window: every clip on every track in one list, and the playlists (alternate takes) of the
// selected track.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "edit/clip_tools.h"
#include "dsp/elastic.h"
#include "instruments/builtin.h"
#include "ui/app.h"

#define WEZT_PAUSED(body) do { const bool _was = deviceRunning_; if (_was) device_->stop(); body; std::string _e; if (_was) device_->start(&_e); } while (0)

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

// ---- clip effects: offline processing of the selected clip into a new file ----
namespace wezt::ui {

void App::drawClipFxWindow() {
  ImGui::SetNextWindowSize(ImVec2(400, 470), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Clip Effects", &showClipFx_)) { ImGui::End(); return; }
  session::Track* tr = session_->track(selTrack_);
  const bool have = tr && selClip_ >= 0;
  if (!have) ImGui::TextDisabled("Select a clip on the timeline first.");
  else ImGui::TextDisabled("Applies to the selected clip, into a new audio file (the original file stays).");
  ImGui::Separator();
  auto run = [&](const char* tag, const char* done, const std::function<bool(AudioBuffer&, double, std::string*)>& fn, bool follow = false) {
    std::string err;
    bool ok = false;
    WEZT_PAUSED(ok = session_->processClip(selTrack_, selClip_, tag, fn, &err, follow));
    setStatus(ok ? std::string(done) : "Clip effect: " + err);
  };
  ImGui::BeginDisabled(!have);
  if (ImGui::Button("Normalize to -1 dBFS", ImVec2(-1, 0)))
    run("norm", "Clip normalized", [](AudioBuffer& b, double, std::string* e) {
      float pk = 0.f;
      for (std::size_t c = 0; c < b.channels; ++c) for (std::size_t i = 0; i < b.frames; ++i) pk = std::max(pk, std::fabs(b.channel(c)[i]));
      if (pk < 1e-6f) { if (e) *e = "the clip is silent"; return false; }
      const float g = std::pow(10.f, -1.f / 20.f) / pk;
      for (std::size_t c = 0; c < b.channels; ++c) for (std::size_t i = 0; i < b.frames; ++i) b.channel(c)[i] *= g;
      return true;
    });
  if (ImGui::Button("Reverse", ImVec2(-1, 0)))
    run("rev", "Clip reversed", [](AudioBuffer& b, double, std::string*) { for (std::size_t c = 0; c < b.channels; ++c) std::reverse(b.channel(c), b.channel(c) + b.frames); return true; });
  if (ImGui::Button("Invert polarity", ImVec2(-1, 0)))
    run("inv", "Polarity inverted", [](AudioBuffer& b, double, std::string*) { for (std::size_t c = 0; c < b.channels; ++c) for (std::size_t i = 0; i < b.frames; ++i) b.channel(c)[i] = -b.channel(c)[i]; return true; });
  ImGui::SetNextItemWidth(160);
  ImGui::SliderFloat("##fxgain", &clipFxGainDb_, -24.f, 24.f, "%.1f dB");
  ImGui::SameLine();
  if (ImGui::Button("Apply gain")) {
    const float db = clipFxGainDb_;
    run("gain", "Gain applied", [db](AudioBuffer& b, double, std::string*) { const float g = std::pow(10.f, db / 20.f); for (std::size_t c = 0; c < b.channels; ++c) for (std::size_t i = 0; i < b.frames; ++i) b.channel(c)[i] *= g; return true; });
  }
  ImGui::Separator();
  ImGui::TextDisabled("Time and pitch (Elastic Audio)");
  {
    const char* modes[] = {"Polyphonic", "Rhythmic", "Monophonic", "Varispeed (pitch follows)"};
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("mode##fxm", &clipFxMode_, modes, 4);
    const auto mode = static_cast<dsp::ElasticMode>(clipFxMode_);
    auto stretch = [&](double ratio, const char* done) {
      run("stretch", done, [ratio, mode](AudioBuffer& b, double rate, std::string* e) {
        AudioBuffer out = dsp::elasticStretch(b, ratio, mode, rate);
        if (out.frames == 0) { if (e) *e = "the stretch is out of range (0.05x to 20x)"; return false; }
        b = std::move(out);
        return true;
      }, true);
    };
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("##fxst", &clipFxStretchPct_, 25.f, 400.f, "length %.0f %%");
    ImGui::SameLine();
    if (ImGui::Button("Stretch")) stretch(static_cast<double>(clipFxStretchPct_) / 100.0, "Clip stretched");
    ImGui::SetNextItemWidth(100);
    ImGui::DragFloat("##fxbpm", &clipFxSrcBpm_, 0.5f, 30.f, 300.f, "%.1f bpm");
    ImGui::SameLine();
    if (ImGui::Button("Fit to project tempo")) stretch(static_cast<double>(clipFxSrcBpm_) / static_cast<double>(std::max(20.f, tempo_)), "Clip fitted to the project tempo");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The clip was played at the tempo on the left; stretch it to the project tempo, same pitch.");
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("##fxsemi", &clipFxSemis_, -12.f, 12.f, "%.1f semitones");
    ImGui::SameLine();
    if (ImGui::Button("Shift pitch")) {
      const double semis = static_cast<double>(clipFxSemis_);
      run("pitch", "Pitch shifted", [semis, mode](AudioBuffer& b, double rate, std::string* e) {
        AudioBuffer out = dsp::elasticPitch(b, semis, mode, rate);
        if (out.frames == 0) { if (e) *e = "could not shift the pitch"; return false; }
        b = std::move(out);
        return true;
      });
    }
  }
  ImGui::Separator();
  ImGui::TextDisabled("Wezt effects, at their default settings");
  for (const auto& e : instruments::builtinPlugins()) {
    if (e.second) continue;   // instruments make no sense here
    if (!ImGui::Button((e.first + "##fx").c_str(), ImVec2(-1, 0))) continue;
    const std::string name = e.first;
    run("fx", (name + " applied").c_str(), [name](AudioBuffer& b, double rate, std::string* err) {
      plugins::PluginDescriptor d; d.name = name; d.format = plugins::Format::Builtin; d.bitness = 64;
      auto fx = instruments::makeBuiltin(d);
      if (!fx) { if (err) *err = "cannot build " + name; return false; }
      constexpr std::size_t kBlock = 256;
      fx->prepare(rate, kBlock);
      const std::size_t lat = fx->latencySamples();
      const unsigned chans = static_cast<unsigned>(std::max<std::size_t>(2, b.channels));
      AudioBuffer in(chans, kBlock), out(chans, kBlock), res(b.channels, b.frames);
      for (std::size_t pos = 0; pos < b.frames + lat; pos += kBlock) {   // run past the end so the latency can be dropped
        for (unsigned c = 0; c < chans; ++c) for (std::size_t i = 0; i < kBlock; ++i) { const std::size_t k = pos + i; in.channel(c)[i] = k < b.frames ? b.channel(std::min<std::size_t>(c, b.channels - 1))[k] : 0.f; }
        fx->process(in, out);
        for (std::size_t c = 0; c < b.channels; ++c) for (std::size_t i = 0; i < kBlock; ++i) { const std::size_t k = pos + i; if (k >= lat && k - lat < b.frames) res.channel(c)[k - lat] = out.channel(c)[i]; }
      }
      for (std::size_t c = 0; c < b.channels; ++c) std::copy(res.channel(c), res.channel(c) + b.frames, b.channel(c));
      return true;
    });
  }
  ImGui::EndDisabled();
  ImGui::End();
}

}  // namespace wezt::ui
