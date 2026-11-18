// Immersive window: speaker layout, object positions (top and side view), estimated speaker levels,
// ADM BWF export and loudness analysis.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "io/wav.h"
#include "ui/app.h"

namespace wezt::ui {
using session::TrackId;
namespace fs = std::filesystem;

namespace {
ImU32 col(float r, float g, float b, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }
const ImU32 kObj[] = {0xFFD9A35A, 0xFF5AA3D9, 0xFF7ACB6B, 0xFF5A8AD9, 0xFFD05AB8, 0xFFC2D05A, 0xFF7A5AD0, 0xFF5AD0A0};
}  // namespace

bool App::exportAdm(const std::string& path, std::string* message) {
  const auto ids = session_->trackIds();
  const std::int64_t end = endOfSession();
  if (ids.empty() || end <= 0) { if (message) *message = "Nothing to export: add clips first"; return false; }
  const bool was = deviceRunning_;
  if (was) device_->stop();
  const fs::path dir = fs::temp_directory_path() / "wezt_adm_stems";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  // Render each track alone, centred, then store it as one mono object channel.
  std::vector<std::pair<TrackId, bool>> soloBefore;
  std::vector<float> panBefore;
  for (TrackId id : ids) { soloBefore.push_back({id, session_->track(id)->solo}); panBefore.push_back(session_->track(id)->strip->pan()); }
  bool ok = true;
  std::vector<std::string> stems;
  for (std::size_t k = 0; k < ids.size() && ok; ++k) {
    for (TrackId id : ids) session_->setSolo(id, id == ids[k]);
    session_->track(ids[k])->strip->setPan(0.f);
    const std::string f = (dir / ("stem" + std::to_string(k) + ".wav")).string();
    const auto r = session_->bounce(f, 0, end, 0, io::WavSampleType::Float32);
    if (!r.ok) { if (message) *message = r.error; ok = false; }
    stems.push_back(f);
  }
  for (std::size_t k = 0; k < ids.size(); ++k) { session_->setSolo(soloBefore[k].first, soloBefore[k].second); session_->track(ids[k])->strip->setPan(panBefore[k]); }
  if (ok) {
    immersive::AdmProgramme prog;
    prog.name = "Wezt Mix";
    prog.sampleRate = session_->sampleRate();
    for (TrackId id : ids) {
      immersive::AdmObjectTrack o;
      o.name = session_->track(id)->name;
      const ObjectPos pos = objects_.count(id) ? objects_[id] : ObjectPos{};
      o.blocks.push_back({0.0, 0.0, pos.x, pos.y, pos.z, 1.f, pos.size});
      prog.objects.push_back(o);
    }
    immersive::AdmWriter w;
    std::string err;
    std::vector<io::WavReader> rd(ids.size());
    for (std::size_t k = 0; k < ids.size() && ok; ++k) if (!rd[k].open(stems[k], &err)) { if (message) *message = err; ok = false; }
    if (ok && !w.open(path, prog, io::WavSampleType::Int24, &err)) { if (message) *message = err; ok = false; }
    if (ok) {
      constexpr std::size_t kBlock = 8192;
      std::vector<std::vector<float>> mono(ids.size(), std::vector<float>(kBlock));
      std::vector<float> l(kBlock), r(kBlock);
      std::vector<const float*> ptrs(ids.size());
      for (std::uint64_t pos = 0; pos < static_cast<std::uint64_t>(end) && ok; pos += kBlock) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, static_cast<std::uint64_t>(end) - pos));
        for (std::size_t k = 0; k < ids.size(); ++k) {
          float* dst[2] = {l.data(), r.data()};
          std::fill(l.begin(), l.end(), 0.f); std::fill(r.begin(), r.end(), 0.f);
          rd[k].read(pos, n, dst);
          const float comp = session_->track(ids[k])->mono ? 1.41421356f : 1.f;   // undo the centre pan law of a mono track
          for (std::size_t i = 0; i < n; ++i) mono[k][i] = 0.5f * (l[i] + r[i]) * comp;
          ptrs[k] = mono[k].data();
        }
        ok = w.write(ptrs.data(), n);
      }
      if (ok) ok = w.finish(&err);
      if (!ok && message && message->empty()) *message = err.empty() ? "write failed" : err;
    }
  }
  fs::remove_all(dir, ec);
  if (was) { std::string e; device_->start(&e); }
  if (ok && message) *message = "Wrote " + std::to_string(ids.size()) + " object channels to " + path;
  return ok;
}

bool App::analyzeMix(std::string* message) {
  const std::int64_t end = endOfSession();
  if (end <= 0) { if (message) *message = "Nothing to measure: add clips first"; return false; }
  const bool was = deviceRunning_;
  if (was) device_->stop();
  const fs::path f = fs::temp_directory_path() / "wezt_loudness.wav";
  const auto r = session_->bounce(f.string(), 0, end, 0, io::WavSampleType::Float32);
  if (was) { std::string e; device_->start(&e); }
  if (!r.ok) { if (message) *message = r.error; return false; }
  io::WavReader rd;
  std::string err;
  if (!rd.open(f.string(), &err)) { if (message) *message = err; return false; }
  immersive::LoudnessMeter m(session_->sampleRate(), immersive::makeLayout("2.0"));
  std::vector<float> l(4800), rr(4800);
  float maxShort = -200.f;
  for (std::uint64_t pos = 0; pos < rd.info().frames; pos += 4800) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(4800, rd.info().frames - pos));
    float* dst[2] = {l.data(), rr.data()};
    rd.read(pos, n, dst);
    const float* p[2] = {l.data(), rr.data()};
    m.feed(p, n);
    maxShort = std::max(maxShort, m.shortTerm());
  }
  rd.close();
  std::error_code ec;
  fs::remove(f, ec);
  char b[200];
  std::snprintf(b, sizeof b, "Integrated %.1f LUFS   short-term max %.1f LUFS   true peak %.1f dBTP   sample peak %.1f dBFS",
                static_cast<double>(m.integrated()), static_cast<double>(maxShort), static_cast<double>(m.truePeakDb()), static_cast<double>(m.samplePeakDb()));
  if (message) *message = b;
  return true;
}

void App::drawImmersiveWindow() {
  ImGui::SetNextWindowSize(ImVec2(1000, 640), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(160, 130), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Immersive panner", &showImmersive_)) { ImGui::End(); return; }
  // layout choice
  ImGui::SetNextItemWidth(110);
  if (ImGui::BeginCombo("Speaker layout", layoutName_.c_str())) {
    for (const auto& n : immersive::layoutNames()) if (ImGui::Selectable(n.c_str(), n == layoutName_)) {
      layoutName_ = n; layout_ = immersive::makeLayout(n);
      panner_ = std::make_unique<immersive::ObjectRenderer>(layout_, immersive::SpeakerLayout{}, 0);
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu speakers, one object per track", layout_.size());

  const auto ids = session_->trackIds();
  std::size_t index = 0;
  for (TrackId id : ids) { if (!objects_.count(id)) { ObjectPos p; p.x = -0.6f + 0.3f * static_cast<float>(index % 5); p.y = 0.8f; objects_[id] = p; } ++index; }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float S = 330.f;
  // speaker levels from object levels
  std::vector<float> spk(layout_.size(), 0.f);
  for (TrackId id : ids) {
    const ObjectPos& o = objects_[id];
    const auto g = panner_->gainsFor(o.x, o.y, o.z, o.size);
    session::Track* t = session_->track(id);
    const auto lv = levels_.count(t->strip) ? levels_[t->strip] : std::array<float, 2>{0.f, 0.f};
    const float level = std::max(lv[0], lv[1]);
    for (std::size_t i = 0; i < spk.size() && i < g.size(); ++i) spk[i] = std::sqrt(spk[i] * spk[i] + g[i] * g[i] * level * level);
  }
  // ---- top view
  ImGui::BeginGroup();
  ImGui::TextDisabled("Top view  (drag objects)");
  const ImVec2 tp = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(tp, ImVec2(tp.x + S, tp.y + S), col(0.07f, 0.08f, 0.1f));
  dl->AddRect(ImVec2(tp.x + 10, tp.y + 10), ImVec2(tp.x + S - 10, tp.y + S - 10), col(1.f, 1.f, 1.f, 0.2f));
  const ImVec2 c(tp.x + S / 2, tp.y + S / 2);
  const float half = S / 2 - 10.f;
  dl->AddCircle(c, half * 0.85f, col(1.f, 1.f, 1.f, 0.1f), 64);
  dl->AddCircleFilled(c, 4.f, col(1.f, 1.f, 1.f, 0.6f));
  dl->AddText(ImVec2(c.x - 14, tp.y + 12), col(1.f, 1.f, 1.f, 0.4f), "front");
  for (std::size_t i = 0; i < layout_.speakers.size(); ++i) {
    const auto& sp = layout_.speakers[i];
    const float az = sp.azimuth * 3.14159265f / 180.f, el = sp.elevation * 3.14159265f / 180.f;
    const float r = sp.lfe ? 0.35f : 0.85f * std::cos(el) + (sp.elevation > 1.f ? -0.12f : 0.f);
    const ImVec2 pos(c.x - std::sin(az) * r * half, c.y - std::cos(az) * r * half);
    const float lvl = std::clamp(spk[i], 0.f, 1.f);
    const float sz = sp.elevation > 1.f ? 9.f : 11.f;
    dl->AddRectFilled(ImVec2(pos.x - sz, pos.y - sz * 0.7f), ImVec2(pos.x + sz, pos.y + sz * 0.7f), col(0.2f + 0.7f * lvl, 0.3f + 0.5f * lvl, 0.35f, 1.f), 3.f);
    if (sp.elevation > 1.f) dl->AddRect(ImVec2(pos.x - sz - 2, pos.y - sz * 0.7f - 2), ImVec2(pos.x + sz + 2, pos.y + sz * 0.7f + 2), col(0.9f, 0.8f, 0.3f, 0.8f), 3.f);
    dl->AddText(ImVec2(pos.x - ImGui::CalcTextSize(sp.label.c_str()).x / 2, pos.y + sz * 0.7f + 1), col(1.f, 1.f, 1.f, 0.8f), sp.label.c_str());
  }
  static TrackId dragObj = -1;
  static bool dragSide = false;
  ImGui::InvisibleButton("##top", ImVec2(S, S));
  const ImVec2 ms = ImGui::GetIO().MousePos;
  if (ImGui::IsItemClicked(0)) {
    dragObj = -1; dragSide = false;
    float best = 16.f;
    for (TrackId id : ids) {
      const ObjectPos& o = objects_[id];
      const float d = std::hypot(ms.x - (c.x + o.x * half), ms.y - (c.y - o.y * half));
      if (d < best) { best = d; dragObj = id; }
    }
    if (dragObj >= 0) selTrack_ = dragObj;
  }
  if (dragObj >= 0 && !dragSide && ImGui::IsMouseDown(0)) {
    ObjectPos& o = objects_[dragObj];
    o.x = std::clamp((ms.x - c.x) / half, -1.f, 1.f);
    o.y = std::clamp(-(ms.y - c.y) / half, -1.f, 1.f);
  }
  index = 0;
  for (TrackId id : ids) {
    const ObjectPos& o = objects_[id];
    const ImVec2 p(c.x + o.x * half, c.y - o.y * half);
    const float rad = 7.f + o.size * 14.f + o.z * 3.f;
    dl->AddCircleFilled(p, rad, (kObj[static_cast<std::size_t>(id) % 8] & 0x00FFFFFF) | 0xC0000000u);
    dl->AddCircle(p, rad, id == selTrack_ ? col(1.f, 1.f, 1.f) : col(0.f, 0.f, 0.f, 0.6f), 0, id == selTrack_ ? 2.f : 1.f);
    char b[8]; std::snprintf(b, sizeof b, "%zu", ++index);
    dl->AddText(ImVec2(p.x - 3, p.y - 6), col(0.f, 0.f, 0.f), b);
  }
  ImGui::EndGroup();
  ImGui::SameLine();
  // ---- side view and speaker bars
  ImGui::BeginGroup();
  ImGui::TextDisabled("Side view  (height)");
  const ImVec2 sp0 = ImGui::GetCursorScreenPos();
  const float SW = 300.f, SH = 130.f;
  dl->AddRectFilled(sp0, ImVec2(sp0.x + SW, sp0.y + SH), col(0.07f, 0.08f, 0.1f));
  dl->AddLine(ImVec2(sp0.x, sp0.y + SH - 14), ImVec2(sp0.x + SW, sp0.y + SH - 14), col(1.f, 1.f, 1.f, 0.25f));
  dl->AddText(ImVec2(sp0.x + 4, sp0.y + 2), col(1.f, 1.f, 1.f, 0.4f), "ceiling");
  dl->AddText(ImVec2(sp0.x + SW - 36, sp0.y + SH - 13), col(1.f, 1.f, 1.f, 0.4f), "front");
  ImGui::InvisibleButton("##side", ImVec2(SW, SH));
  auto sidePos = [&](const ObjectPos& o) { return ImVec2(sp0.x + 10 + (1.f - (o.y * 0.5f + 0.5f)) * (SW - 20), sp0.y + SH - 14 - o.z * (SH - 28)); };
  if (ImGui::IsItemClicked(0)) {
    dragObj = -1; dragSide = true;
    float best = 16.f;
    for (TrackId id : ids) { const ImVec2 p = sidePos(objects_[id]); const float d = std::hypot(ms.x - p.x, ms.y - p.y); if (d < best) { best = d; dragObj = id; } }
    if (dragObj >= 0) selTrack_ = dragObj;
  }
  if (dragObj >= 0 && dragSide && ImGui::IsMouseDown(0)) objects_[dragObj].z = std::clamp((sp0.y + SH - 14 - ms.y) / (SH - 28), 0.f, 1.f);
  if (!ImGui::IsMouseDown(0)) dragObj = -1;
  for (TrackId id : ids) { const ImVec2 p = sidePos(objects_[id]); dl->AddCircleFilled(p, 6.f, kObj[static_cast<std::size_t>(id) % 8]); }
  ImGui::TextDisabled("Speaker levels (estimated)");
  const ImVec2 bp = ImGui::GetCursorScreenPos();
  const float bw = SW / static_cast<float>(layout_.size());
  for (std::size_t i = 0; i < layout_.size(); ++i) {
    const float h = 70.f * std::clamp(spk[i], 0.f, 1.f);
    dl->AddRectFilled(ImVec2(bp.x + bw * static_cast<float>(i) + 2, bp.y), ImVec2(bp.x + bw * static_cast<float>(i + 1) - 2, bp.y + 70), col(0.07f, 0.08f, 0.1f));
    dl->AddRectFilled(ImVec2(bp.x + bw * static_cast<float>(i) + 2, bp.y + 70 - h), ImVec2(bp.x + bw * static_cast<float>(i + 1) - 2, bp.y + 70), col(0.3f, 0.8f, 0.5f));
    dl->AddText(ImVec2(bp.x + bw * static_cast<float>(i) + 2, bp.y + 72), col(1.f, 1.f, 1.f, 0.7f), layout_.speakers[i].label.c_str());
  }
  ImGui::Dummy(ImVec2(SW, 90));
  ImGui::EndGroup();

  ImGui::Separator();
  // ---- per-object controls
  if (ImGui::BeginTable("objs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Object"); ImGui::TableSetupColumn("X (left - right)"); ImGui::TableSetupColumn("Y (back - front)");
    ImGui::TableSetupColumn("Z (height)"); ImGui::TableSetupColumn("Size"); ImGui::TableSetupColumn("Level");
    ImGui::TableHeadersRow();
    index = 0;
    for (TrackId id : ids) {
      ObjectPos& o = objects_[id];
      ImGui::PushID(id);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable((std::to_string(++index) + "  " + session_->track(id)->name).c_str(), id == selTrack_, ImGuiSelectableFlags_SpanAllColumns)) selTrack_ = id;
      ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##x", &o.x, -1.f, 1.f, "%.2f");
      ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##y", &o.y, -1.f, 1.f, "%.2f");
      ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##z", &o.z, 0.f, 1.f, "%.2f");
      ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##s", &o.size, 0.f, 1.f, "%.2f");
      ImGui::TableNextColumn();
      float db = session_->track(id)->strip->gainDb();
      ImGui::SetNextItemWidth(-1);
      if (ImGui::SliderFloat("##g", &db, -60.f, 12.f, "%.1f dB")) session_->track(id)->strip->setGainDb(db);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  // ---- export and loudness
  ImGui::SetNextItemWidth(320);
  ImGui::InputText("ADM BWF file", &admPath_);
  ImGui::SameLine();
  if (ImGui::Button("Export ADM BWF")) { std::string m; exportAdm(admPath_, &m); setStatus(m); loudnessText_ = m; }
  ImGui::SameLine();
  if (ImGui::Button("Measure loudness")) { std::string m; analyzeMix(&m); loudnessText_ = m; setStatus(m); }
  if (!loudnessText_.empty()) ImGui::TextWrapped("%s", loudnessText_.c_str());
  ImGui::TextWrapped("Objects are mono (a stereo track is summed). Speaker levels are estimated from the track meters; live immersive monitoring through the audio engine is not wired yet.");
  ImGui::End();
}

}  // namespace wezt::ui
