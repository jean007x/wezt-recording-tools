// Sampler editor: mode tabs, slice waveform, key tags, slice strip, LFO / pitch / filter / amp panels, keyboard.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "io/wav.h"
#include "ui/app.h"
#include "ui/style.h"

namespace wezt::ui {
using instruments::FilterType;
using instruments::LfoShape;
using instruments::LfoVia;
using instruments::LoopMode;
using instruments::Sampler;
using instruments::SamplerMode;
using instruments::SamplerParams;
using instruments::Zone;
namespace st = style;

namespace {
ImU32 col(float r, float g, float b, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }
const char* noteName(int key) {
  static char b[4][8];
  static int n = 0;
  char* o = b[n++ & 3];
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  std::snprintf(o, 8, "%s%d", names[((key % 12) + 12) % 12], key / 12 - 2);
  return o;
}
bool isBlack(int k) { const int m = k % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; }
std::string baseName(const std::string& path) {
  const auto s = path.find_last_of("/\\");
  return s == std::string::npos ? path : path.substr(s + 1);
}
void caption(const char* t) { ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.60f, 0.62f, 0.67f)); ImGui::TextUnformatted(t); ImGui::PopStyleColor(); }
bool sameAudio(const std::vector<Zone>& zs) {
  for (const Zone& z : zs) if (z.audio != zs.front().audio) return false;
  return true;
}
// Panels draw a title and keep their own padding.
void panelTitle(const char* t) {
  ImGui::PushStyleColor(ImGuiCol_Text, st::rgba(0.82f, 0.84f, 0.88f));
  ImGui::TextUnformatted(t);
  ImGui::PopStyleColor();
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// slicing and mode helpers
void App::samplerRemapKeys(Sampler* s) {
  std::vector<Zone> zs = s->zones();
  if (zs.empty()) return;
  int key = startKey_;
  std::vector<Zone> out;
  for (Zone& z : zs) {
    if (!chromatic_) while (key <= 127 && isBlack(key)) ++key;
    if (key > 127) break;
    z.keyLo = z.keyHi = z.rootKey = key++;
    z.oneShot = playToEnd_;
    out.push_back(z);
  }
  s->clearZones();
  for (Zone& z : out) s->addZone(z);
}

void App::samplerReslice(Sampler* s) {
  if (!sliceSource_) return;
  s->clearZones();
  if (sliceMode_ == 0) s->sliceTransients(sliceSource_, sliceRate_, sliceSens_, 64, startKey_);
  else s->sliceEqual(sliceSource_, sliceRate_, sliceCount_, startKey_);
  samplerRemapKeys(s);
  selZone_ = 0;
}

void App::samplerSetMode(Sampler* s, SamplerMode m) {
  SamplerParams q = s->params();
  q.mode = m;
  s->setParams(q);
  std::vector<Zone> zs = s->zones();
  if (zs.empty()) return;
  if (m == SamplerMode::Slice) {
    if (zs.size() == 1 || !sameAudio(zs) || !sliceSource_) { sliceSource_ = zs[0].audio; sliceRate_ = zs[0].sampleRate; samplerReslice(s); }
    return;
  }
  if (zs.size() > 1 && sameAudio(zs) && sliceSource_) {   // back from slices to one pitched sample
    Zone z;
    z.audio = sliceSource_; z.sampleRate = sliceRate_; z.file = zs[0].file;
    z.rootKey = 60; z.keyLo = 0; z.keyHi = 127;
    s->clearZones();
    zs.assign(1, z);
    s->addZone(z);
    selZone_ = 0;
  }
  if (zs.size() == 1) {
    Zone z = s->zones()[0];
    z.oneShot = m == SamplerMode::OneShot;
    z.keyLo = 0; z.keyHi = 127;
    s->updateZone(0, z);
  }
}

// ---------------------------------------------------------------------------------------------
bool App::loadSelectedClipIntoSampler(Sampler* s) {
  session::Track* tr = session_->track(selTrack_);
  if (!s || !tr || selClip_ < 0) { setStatus("Select a clip on the timeline first"); return false; }
  for (const auto& c : tr->source->playlist()) {
    if (c.id != selClip_ || !c.audio) continue;
    const std::int64_t avail = static_cast<std::int64_t>(c.audio->frames);
    const std::int64_t from = std::clamp<std::int64_t>(c.offset, 0, avail), to = std::clamp<std::int64_t>(c.offset + c.length, from, avail);
    if (to <= from) break;
    AudioBuffer buf(c.audio->channels, static_cast<std::size_t>(to - from));
    for (std::size_t ch = 0; ch < buf.channels; ++ch) std::copy(c.audio->channel(ch) + from, c.audio->channel(ch) + to, buf.channel(ch));
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(session_->recordFolder());
    std::filesystem::create_directories(dir, ec);
    std::string stem = c.name.empty() ? "clip" : c.name;
    for (char& ch : stem) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
    std::filesystem::path out = dir / (stem + "_sample.wav");
    for (int n = 2; std::filesystem::exists(out); ++n) out = dir / (stem + "_sample_" + std::to_string(n) + ".wav");
    if (!io::writeWavFloat(out.string(), buf, static_cast<unsigned>(session_->sampleRate()))) { setStatus("Could not write " + out.string()); return false; }
    Zone z; std::string err;
    if (!s->loadSampleFile(out.string(), z, &err)) { setStatus("Could not load: " + err); return false; }
    z.rootKey = 60; z.keyLo = 0; z.keyHi = 127;
    z.oneShot = s->params().mode == SamplerMode::OneShot;
    s->clearZones(); selZone_ = 0;
    sliceSource_ = z.audio; sliceRate_ = z.sampleRate;
    s->addZone(z);
    if (s->params().mode == SamplerMode::Slice) samplerReslice(s);
    viewStart_ = 0.f;
    setStatus("Loaded clip \"" + c.name + "\" into the sampler");
    return true;
  }
  setStatus("Select a clip on the timeline first");
  return false;
}

void App::drawZoneWindow(Sampler* s) {
  ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Sampler zones", &showZones_)) { ImGui::End(); return; }
  std::vector<Zone> zones = s->zones();
  ImGui::TextDisabled("Multi-sample mapping: several samples across keys and velocities, with round robin.");
  ImGui::SetNextItemWidth(340);
  ImGui::InputTextWithHint("##zp", "path to a WAV file", &samplerPath_);
  ImGui::SameLine();
  if (ImGui::Button("Add zone")) {
    Zone z; std::string err;
    if (s->loadSampleFile(samplerPath_, z, &err)) {
      const int root = Sampler::noteNameToKey(samplerPath_);
      z.rootKey = root >= 0 ? root : 60; z.keyLo = z.keyHi = z.rootKey; if (root < 0) { z.keyLo = 0; z.keyHi = 127; }
      selZone_ = s->addZone(z);
      setStatus("Added zone from " + samplerPath_);
    } else setStatus("Could not load: " + err);
  }
  if (selZone_ >= static_cast<int>(zones.size())) selZone_ = static_cast<int>(zones.size()) - 1;
  if (zones.empty()) { ImGui::TextDisabled("No zones."); ImGui::End(); return; }
  if (selZone_ < 0) selZone_ = 0;
  ImGui::SetNextItemWidth(200);
  if (ImGui::BeginCombo("zone", (std::to_string(selZone_ + 1) + ": " + baseName(zones[static_cast<std::size_t>(selZone_)].file)).c_str())) {
    for (std::size_t i = 0; i < zones.size(); ++i) if (ImGui::Selectable((std::to_string(i + 1) + ": " + baseName(zones[i].file) + " (" + noteName(zones[i].keyLo) + "-" + noteName(zones[i].keyHi) + ")").c_str(), static_cast<int>(i) == selZone_)) selZone_ = static_cast<int>(i);
    ImGui::EndCombo();
  }
  Zone z = zones[static_cast<std::size_t>(selZone_)];
  bool zc = false;
  const int frames = z.audio ? static_cast<int>(z.audio->frames) : 0;
  int stt = static_cast<int>(z.start), en = z.end < 0 ? frames : static_cast<int>(z.end), ls = static_cast<int>(z.loopStart), le = z.loopEnd < 0 ? frames : static_cast<int>(z.loopEnd);
  ImGui::SetNextItemWidth(110); if (ImGui::DragInt("start", &stt, 10.f, 0, std::max(1, frames))) { z.start = stt; zc = true; } ImGui::SameLine();
  ImGui::SetNextItemWidth(110); if (ImGui::DragInt("end", &en, 10.f, 0, std::max(1, frames))) { z.end = en; zc = true; }
  int lm = static_cast<int>(z.loop);
  ImGui::SetNextItemWidth(110); if (ImGui::Combo("loop", &lm, "Off\0Forward\0Ping-pong\0")) { z.loop = static_cast<LoopMode>(lm); zc = true; }
  if (z.loop != LoopMode::Off) {
    ImGui::SetNextItemWidth(110); if (ImGui::DragInt("loop start", &ls, 10.f, 0, std::max(1, frames))) { z.loopStart = ls; zc = true; } ImGui::SameLine();
    ImGui::SetNextItemWidth(110); if (ImGui::DragInt("loop end", &le, 10.f, 0, std::max(1, frames))) { z.loopEnd = le; zc = true; }
    int xf = static_cast<int>(z.loopCrossfade);
    ImGui::SetNextItemWidth(110); if (ImGui::DragInt("crossfade", &xf, 5.f, 0, 20000)) { z.loopCrossfade = xf; zc = true; }
  }
  zc |= ImGui::Checkbox("reverse", &z.reverse); ImGui::SameLine(); zc |= ImGui::Checkbox("one shot", &z.oneShot);
  ImGui::SetNextItemWidth(80); zc |= ImGui::DragInt("root", &z.rootKey, 0.2f, 0, 127, noteName(z.rootKey)); ImGui::SameLine();
  ImGui::SetNextItemWidth(70); zc |= ImGui::DragInt("key lo", &z.keyLo, 0.2f, 0, 127, noteName(z.keyLo)); ImGui::SameLine();
  ImGui::SetNextItemWidth(70); zc |= ImGui::DragInt("hi", &z.keyHi, 0.2f, 0, 127, noteName(z.keyHi));
  ImGui::SetNextItemWidth(70); zc |= ImGui::DragInt("vel lo", &z.velLo, 0.2f, 1, 127); ImGui::SameLine();
  ImGui::SetNextItemWidth(70); zc |= ImGui::DragInt("vel hi", &z.velHi, 0.2f, 1, 127);
  ImGui::SetNextItemWidth(110); zc |= ImGui::DragFloat("tune (cents)", &z.tuneCents, 0.5f, -1200.f, 1200.f, "%.1f"); ImGui::SameLine();
  ImGui::SetNextItemWidth(110); zc |= ImGui::DragFloat("gain (dB)", &z.gainDb, 0.1f, -60.f, 24.f, "%.1f");
  ImGui::SetNextItemWidth(110); zc |= ImGui::SliderFloat("pan", &z.pan, -1.f, 1.f, "%.2f"); ImGui::SameLine();
  ImGui::SetNextItemWidth(60); zc |= ImGui::DragInt("round-robin group", &z.rrGroup, 0.1f, 0, 63);
  if (z.rrGroup > 0) { ImGui::SameLine(); ImGui::SetNextItemWidth(60); zc |= ImGui::DragInt("index", &z.rrIndex, 0.1f, 0, 63); }
  if (zc) s->updateZone(static_cast<std::size_t>(selZone_), z);
  if (ImGui::Button("Remove zone")) s->removeZone(static_cast<std::size_t>(selZone_));
  ImGui::End();
}

// ---------------------------------------------------------------------------------------------
void App::drawSamplerWindow() {
  ImGui::SetNextWindowSize(ImVec2(1240, 800), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(24, 76), ImGuiCond_FirstUseEver);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, st::rgba(0.125f, 0.130f, 0.145f));
  const bool open = ImGui::Begin("Sampler", &showSampler_);
  ImGui::PopStyleColor();
  if (!open) { ImGui::End(); return; }
  Sampler* s = samplerOn(selTrack_);
  if (!s) {
    session::Track* t = session_->track(selTrack_);
    ImGui::Text("Track \"%s\" has no sampler.", t ? t->name.c_str() : "-");
    if (t && ImGui::Button("Add a sampler to this track")) {
      const bool was = deviceRunning_; if (was) device_->stop();
      session_->addInsert(selTrack_, std::make_unique<Sampler>(), Sampler::descriptor(), 0);
      if (was) { std::string e; device_->start(&e); }
    }
    ImGui::End();
    return;
  }
  std::vector<Zone> zones = s->zones();
  SamplerParams p = s->params();
  if (p.mode == SamplerMode::Slice && !sliceSource_ && !zones.empty() && sameAudio(zones)) { sliceSource_ = zones[0].audio; sliceRate_ = zones[0].sampleRate; }   // a loaded patch or session
  bool pc = false;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const double rate = session_->sampleRate();
  const float fullW = ImGui::GetContentRegionAvail().x;
  const ImU32 gold = st::kGold;
  const ImU32 goldText = 0xFF14100C;

  // ============================ top bar =====================================================
  {
    const SamplerMode modes[] = {SamplerMode::Classic, SamplerMode::OneShot, SamplerMode::Slice};
    const char* names[] = {"CLASSIC", "ONE SHOT", "SLICE"};
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine(0, 0);
      if (st::chip(names[i], p.mode == modes[i], gold, ImVec2(86, 28), goldText)) { samplerSetMode(s, modes[i]); p = s->params(); }
    }
    ImGui::SameLine(0, 10);
    if (st::chip("FROM CLIP", false, gold, ImVec2(92, 28))) loadSelectedClipIntoSampler(s);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Loads the clip selected on the timeline as the sample (replaces the current one).\nRecord on a track first, then select the take.");
    const std::string fname = zones.empty() ? "No sample loaded" : baseName(zones[0].file.empty() ? std::string("(sliced sample)") : zones[0].file);
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 8.f, (ImGui::GetWindowWidth() - ImGui::CalcTextSize(fname.c_str()).x) * 0.5f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(fname.c_str());
    // right side: snap, zoom, gear
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 470.f);
    ImGui::AlignTextToFramePadding(); caption("Snap:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(96); ImGui::Combo("##snap", &snapMode_, "Off\0Transient\0Grid\0");
    ImGui::SameLine(0, 14); ImGui::AlignTextToFramePadding(); caption("Zoom:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(70); ImGui::SliderFloat("##zh", &zoomH_, 1.f, 64.f, "H %.0fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(60); ImGui::SliderFloat("##zv", &zoomV_, 1.f, 8.f, "V %.1fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine(0, 10);
    {
      const ImVec2 gp = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##gear", ImVec2(42, 28))) ImGui::OpenPopup("##gearmenu");
      st::face(dl, gp, ImVec2(gp.x + 42, gp.y + 28), 0xFF3C3E44, false, ImGui::IsItemHovered());
      const ImVec2 c(gp.x + 15, gp.y + 14);
      dl->AddCircle(c, 5.f, 0xFFD8DADF, 12, 2.f);
      for (int k = 0; k < 8; ++k) { const float a = static_cast<float>(k) * 0.7853982f; dl->AddLine(ImVec2(c.x + std::cos(a) * 6.f, c.y + std::sin(a) * 6.f), ImVec2(c.x + std::cos(a) * 9.f, c.y + std::sin(a) * 9.f), 0xFFD8DADF, 2.2f); }
      dl->AddTriangleFilled(ImVec2(gp.x + 28, gp.y + 11), ImVec2(gp.x + 36, gp.y + 11), ImVec2(gp.x + 32, gp.y + 17), 0xFFD8DADF);
    }
    if (ImGui::BeginPopup("##gearmenu")) {
      ImGui::SetNextItemWidth(360);
      ImGui::InputTextWithHint("##sp", "path to a WAV file or a patch", &samplerPath_);
      if (ImGui::MenuItem("Load this sample (replaces the current one)")) {
        Zone z; std::string err;
        if (s->loadSampleFile(samplerPath_, z, &err)) {
          const int root = Sampler::noteNameToKey(samplerPath_);
          z.rootKey = root >= 0 ? root : 60; z.keyLo = 0; z.keyHi = 127;
          z.oneShot = p.mode == SamplerMode::OneShot;
          s->clearZones(); selZone_ = 0;
          sliceSource_ = z.audio; sliceRate_ = z.sampleRate;
          s->addZone(z);
          if (p.mode == SamplerMode::Slice) samplerReslice(s);
          viewStart_ = 0.f;
          setStatus("Loaded " + samplerPath_);
        } else setStatus("Could not load: " + err);
      }
      if (ImGui::MenuItem("Save patch to this path")) {
        FILE* f = std::fopen(samplerPath_.c_str(), "wb");
        if (f) { const std::string t = s->savePatch(); std::fwrite(t.data(), 1, t.size(), f); std::fclose(f); setStatus("Saved patch"); } else setStatus("Cannot write that path");
      }
      if (ImGui::MenuItem("Load patch from this path")) {
        FILE* f = std::fopen(samplerPath_.c_str(), "rb");
        if (f) {
          std::string t; char buf[4096]; std::size_t n;
          while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n);
          std::fclose(f);
          std::string warn;
          setStatus(s->loadPatch(t, &warn) ? (warn.empty() ? "Patch loaded" : "Patch loaded; " + warn) : "Not a sampler patch: " + warn);
          const auto zs2 = s->zones();
          if (!zs2.empty()) { sliceSource_ = zs2[0].audio; sliceRate_ = zs2[0].sampleRate; }
          p = s->params();
        } else setStatus("Cannot read that path");
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Multi-sample zones...")) showZones_ = true;
      if (ImGui::MenuItem("Clear")) { s->clearZones(); sliceSource_.reset(); selZone_ = 0; }
      ImGui::EndPopup();
    }
  }
  ImGui::Dummy(ImVec2(0, 2));

  // ============================ waveform =====================================================
  const float waveH = 220.f, flagH = 20.f;
  const ImVec2 wp = ImGui::GetCursorScreenPos();
  const ImVec2 wq(wp.x + fullW, wp.y + waveH);
  dl->AddRectFilled(wp, wq, st::kNavy, 3.f);
  dl->AddRect(wp, wq, st::rgba(0.f, 0.f, 0.f, 0.9f), 3.f);
  std::shared_ptr<const AudioBuffer> audio = p.mode == SamplerMode::Slice && sliceSource_ ? sliceSource_ : (zones.empty() ? nullptr : zones[static_cast<std::size_t>(std::clamp(selZone_, 0, static_cast<int>(zones.size()) - 1))].audio);
  const bool sliced = p.mode == SamplerMode::Slice && audio && !zones.empty();
  ImGui::InvisibleButton("##wave", ImVec2(fullW, waveH));
  const bool waveHover = ImGui::IsItemHovered();
  if (!audio || audio->frames == 0) {
    const char* msg = zones.empty() ? "No sample. Open the menu at the top right, type a WAV path and choose \"Load this sample\"." : "This zone's sample file is missing.";
    dl->AddText(ImVec2(wp.x + 24, wp.y + waveH * 0.5f - 8), st::rgba(0.62f, 0.68f, 0.80f), msg);
  } else {
    const double frames = static_cast<double>(audio->frames);
    zoomH_ = std::clamp(zoomH_, 1.f, 64.f);
    const double visible = frames / static_cast<double>(zoomH_);
    viewStart_ = std::clamp(viewStart_, 0.f, static_cast<float>(1.0 - 1.0 / static_cast<double>(zoomH_)));
    const double startF = static_cast<double>(viewStart_) * frames;
    const double fpp = visible / static_cast<double>(fullW);
    auto fx = [&](std::int64_t f) { return wp.x + static_cast<float>((static_cast<double>(f) - startF) / fpp); };
    auto xf = [&](float x) { return static_cast<std::int64_t>(startF + static_cast<double>(x - wp.x) * fpp); };
    ImGuiIO& io = ImGui::GetIO();
    if (waveHover && io.MouseWheel != 0.f) {
      if (io.KeyCtrl) { zoomH_ = std::clamp(zoomH_ * (io.MouseWheel > 0 ? 1.25f : 0.8f), 1.f, 64.f); }
      else viewStart_ = std::clamp(viewStart_ - io.MouseWheel * 0.08f / zoomH_, 0.f, static_cast<float>(1.0 - 1.0 / static_cast<double>(zoomH_)));
    }
    const PeakCache& pk = peaksFor(audio);
    const bool stereo = !pk.lo2.empty();
    const float top = wp.y + flagH, laneArea = waveH - flagH - 4.f, laneH = stereo ? laneArea * 0.5f : laneArea;
    // beat grid
    {
      const double beat = rate * 60.0 / static_cast<double>(std::max(30.f, tempo_));
      if (beat / fpp > 8.0) for (double f = std::ceil(startF / beat) * beat; f < startF + visible; f += beat) {
        const float x = fx(static_cast<std::int64_t>(f));
        dl->AddLine(ImVec2(x, top), ImVec2(x, wq.y - 2.f), st::rgba(1.f, 1.f, 1.f, 0.05f));
      }
    }
    // inactive regions (outside start/end) are dimmed later; first the lanes
    for (int lane = 0; lane < (stereo ? 2 : 1); ++lane) {
      const float mid = top + laneH * (static_cast<float>(lane) + 0.5f), amp = laneH * 0.5f * zoomV_;
      const auto& L = lane == 0 ? pk.lo : pk.lo2; const auto& H = lane == 0 ? pk.hi : pk.hi2;
      dl->PushClipRect(ImVec2(wp.x, top + laneH * static_cast<float>(lane)), ImVec2(wq.x, top + laneH * static_cast<float>(lane + 1)), true);
      dl->AddLine(ImVec2(wp.x, mid), ImVec2(wq.x, mid), st::rgba(1.f, 1.f, 1.f, 0.16f));
      for (float x = 0; x < fullW; x += 1.f) {
        const double f0 = startF + static_cast<double>(x) * fpp, f1 = f0 + fpp;
        if (f0 >= frames) break;
        const std::size_t b0 = static_cast<std::size_t>(f0 / PeakCache::kBucket), b1 = std::min(L.size(), static_cast<std::size_t>(f1 / PeakCache::kBucket) + 1);
        float lo = 0, hi = 0;
        for (std::size_t k = b0; k < b1; ++k) { lo = std::min(lo, L[k]); hi = std::max(hi, H[k]); }
        dl->AddLine(ImVec2(wp.x + x, mid - hi * amp), ImVec2(wp.x + x, mid - lo * amp + 1.f), st::rgba(0.36f, 0.68f, 1.f));
      }
      dl->PopClipRect();
      if (lane == 0 && stereo) dl->AddLine(ImVec2(wp.x, top + laneH), ImVec2(wq.x, top + laneH), st::rgba(0.f, 0.f, 0.f, 0.6f));
    }
    // markers
    struct Marker { int id; std::int64_t frame; bool edge; ImU32 color; };
    std::vector<Marker> marks;
    const std::int64_t total = static_cast<std::int64_t>(audio->frames);
    const ImU32 blue = st::rgba(0.30f, 0.55f, 1.f), yellow = gold;
    if (sliced) {
      for (std::size_t i = 0; i < zones.size(); ++i) marks.push_back({100 + static_cast<int>(i), zones[i].start, i == 0, i == 0 ? blue : yellow});
      marks.push_back({100 + static_cast<int>(zones.size()), zones.back().end < 0 ? total : zones.back().end, true, blue});
    } else if (!zones.empty()) {
      const Zone& z = zones[static_cast<std::size_t>(std::clamp(selZone_, 0, static_cast<int>(zones.size()) - 1))];
      marks.push_back({0, z.start, true, st::rgba(0.3f, 0.9f, 0.4f)});
      marks.push_back({1, z.end < 0 ? total : z.end, true, st::rgba(0.95f, 0.35f, 0.3f)});
      if (z.loop != LoopMode::Off) { marks.push_back({2, z.loopStart, false, yellow}); marks.push_back({3, z.loopEnd < 0 ? total : z.loopEnd, false, yellow}); }
      if (z.loop != LoopMode::Off) {
        const std::int64_t le = z.loopEnd < 0 ? total : z.loopEnd;
        dl->AddRectFilled(ImVec2(std::max(wp.x, fx(z.loopStart)), top), ImVec2(std::min(wq.x, fx(le)), wq.y - 2.f), st::shade(yellow, 1.f, 38));
      }
      dl->AddRectFilled(wp, ImVec2(std::clamp(fx(z.start), wp.x, wq.x), wq.y), st::rgba(0.f, 0.f, 0.f, 0.35f));
      dl->AddRectFilled(ImVec2(std::clamp(fx(z.end < 0 ? total : z.end), wp.x, wq.x), wp.y), wq, st::rgba(0.f, 0.f, 0.f, 0.35f));
    }
    // snap targets
    if (snapMode_ == 1 && snapFor_ != audio.get()) { snapCuts_ = Sampler::findTransients(*audio, sliceRate_ > 0 ? sliceRate_ : rate, 1.f, 512); snapFor_ = audio.get(); }
    auto snapF = [&](std::int64_t f) {
      const double tol = 9.0 * fpp;
      if (snapMode_ == 1) { std::int64_t best = f; double bd = tol; for (std::int64_t c : snapCuts_) { const double d = std::fabs(static_cast<double>(c - f)); if (d < bd) { bd = d; best = c; } } return best; }
      if (snapMode_ == 2) { const double beat = rate * 60.0 / static_cast<double>(std::max(30.f, tempo_)); const std::int64_t g = static_cast<std::int64_t>(std::llround(static_cast<double>(f) / beat) * beat); return std::fabs(static_cast<double>(g - f)) < tol ? g : f; }
      return f;
    };
    // hit test and dragging
    const ImVec2 ms = io.MousePos;
    int hot = -1;
    if (dragMarker_ < 0 && waveHover) for (const Marker& m : marks) if (std::fabs(ms.x - fx(m.frame)) < 6.f && ms.y < wq.y) { hot = m.id; break; }
    if (hot >= 0 && ImGui::IsMouseClicked(0)) dragMarker_ = hot;
    if (dragMarker_ >= 0) {
      if (!ImGui::IsMouseDown(0)) dragMarker_ = -1;
      else {
        std::int64_t f = std::clamp<std::int64_t>(snapF(xf(ms.x)), 0, total);
        const int id = dragMarker_;
        if (id >= 100) {
          const int i = id - 100, n = static_cast<int>(zones.size());
          const std::int64_t lowB = i > 0 ? zones[static_cast<std::size_t>(i - 1)].start + 64 : 0;
          const std::int64_t highB = i < n ? (zones[static_cast<std::size_t>(i)].end < 0 ? total : zones[static_cast<std::size_t>(i)].end) - 64 : total;
          const std::int64_t lim = (i == n) ? std::max(lowB, zones.back().start + 64) : lowB;
          f = std::clamp(f, lim, std::max(lim, i == n ? total : highB));
          if (i < n) { Zone z = zones[static_cast<std::size_t>(i)]; z.start = f; s->updateZone(static_cast<std::size_t>(i), z); }
          if (i > 0) { Zone z = zones[static_cast<std::size_t>(i - 1)]; z.end = f; s->updateZone(static_cast<std::size_t>(i - 1), z); }
        } else if (!zones.empty()) {
          const std::size_t zi = static_cast<std::size_t>(std::clamp(selZone_, 0, static_cast<int>(zones.size()) - 1));
          Zone z = zones[zi];
          const std::int64_t zEnd = z.end < 0 ? total : z.end;
          if (id == 0) z.start = std::min(f, zEnd - 64);
          else if (id == 1) z.end = std::max(f, z.start + 64);
          else if (id == 2) z.loopStart = std::clamp(f, z.start, zEnd - 2);
          else if (id == 3) z.loopEnd = std::clamp(f, z.start + 2, zEnd);
          s->updateZone(zi, z);
        }
      }
    }
    if (hot >= 0 || dragMarker_ >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    // flags and numbers
    for (const Marker& m : marks) {
      const float x = fx(m.frame);
      if (x < wp.x - 8 || x > wq.x + 8) continue;
      const bool lit = m.id == hot || m.id == dragMarker_;
      const ImU32 c = lit ? st::shade(m.color, 1.25f) : m.color;
      dl->AddLine(ImVec2(x, wp.y + 4), ImVec2(x, wq.y - 2.f), c, lit ? 2.f : 1.f);
      dl->AddRectFilled(ImVec2(x, wp.y + 3), ImVec2(x + 11, wp.y + 14), c, 1.5f);
      dl->AddTriangleFilled(ImVec2(x, wp.y + 14), ImVec2(x + 5, wp.y + 14), ImVec2(x, wp.y + 19), c);
      if (sliced && m.id >= 100 && m.id < 100 + static_cast<int>(zones.size())) { char nb[8]; std::snprintf(nb, sizeof nb, "%d", m.id - 99); dl->AddText(ImVec2(x + 2, wp.y + 17), st::rgba(0.75f, 0.82f, 0.95f), nb); }
    }
    if (waveHover && !sliced && ImGui::IsMouseClicked(0) && hot < 0 && !zones.empty()) {}
  }
  ImGui::Dummy(ImVec2(0, 2));

  // ============================ key tags (slice) / hint ======================================
  {
    const ImVec2 kp = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(fullW, 24.f));
    if (sliced && audio) {
      const double frames = static_cast<double>(audio->frames), visible = frames / static_cast<double>(zoomH_);
      const double startF = static_cast<double>(viewStart_) * frames, fpp = visible / static_cast<double>(fullW);
      for (std::size_t i = 0; i < zones.size(); ++i) {
        const float x0 = kp.x + static_cast<float>((static_cast<double>(zones[i].start) - startF) / fpp);
        const float x1 = kp.x + static_cast<float>(((zones[i].end < 0 ? frames : static_cast<double>(zones[i].end)) - startF) / fpp);
        const float a = std::max(x0 + 1.f, kp.x + 22.f), b = std::min(x1 - 2.f, kp.x + fullW - 22.f);
        if (b - a < 8.f) continue;
        dl->AddRectFilled(ImVec2(a, kp.y + 2), ImVec2(b, kp.y + 22), st::rgba(0.17f, 0.36f, 0.72f), 4.f);
        const char* nm = noteName(zones[i].rootKey);
        const ImVec2 ts = ImGui::CalcTextSize(nm);
        if (b - a > ts.x + 4.f) dl->AddText(ImVec2((a + b - ts.x) * 0.5f, kp.y + 4), 0xFFFFFFFF, nm);
      }
      ImGui::SetCursorScreenPos(ImVec2(kp.x, kp.y + 2));
      if (st::chip("<", false, gold, ImVec2(18, 20))) { startKey_ = std::max(0, startKey_ - 1); samplerRemapKeys(s); }
      ImGui::SetCursorScreenPos(ImVec2(kp.x + fullW - 18, kp.y + 2));
      if (st::chip(">", false, gold, ImVec2(18, 20))) { startKey_ = std::min(127, startKey_ + 1); samplerRemapKeys(s); }
      ImGui::SetCursorScreenPos(ImVec2(kp.x, kp.y + 24));
    } else if (!zones.empty()) {
      dl->AddText(ImVec2(kp.x + 4, kp.y + 5), st::rgba(0.60f, 0.62f, 0.67f), p.mode == SamplerMode::OneShot ? "One Shot: the sample plays through at the pitch of the key and ignores key release." : "Classic: the sample is pitched across the keyboard; set a loop to sustain it.");
    }
  }

  // ============================ control strip ================================================
  {
    ImGui::AlignTextToFramePadding();
    if (sliced) {
      caption("Mode"); ImGui::SameLine();
      ImGui::SetNextItemWidth(100);
      if (ImGui::Combo("##smode", &sliceMode_, "Transient\0Equal\0")) samplerReslice(s);
      ImGui::SameLine(0, 14);
      if (sliceMode_ == 0) {
        caption("Sensitivity"); ImGui::SameLine();
        float sens = sliceSens_ * 100.f;
        ImGui::SetNextItemWidth(140);
        if (ImGui::SliderFloat("##sens", &sens, 0.f, 100.f, "%.0f")) { sliceSens_ = sens / 100.f; samplerReslice(s); }
      } else {
        caption("Slices"); ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::DragInt("##cnt", &sliceCount_, 0.2f, 2, 64)) samplerReslice(s);
      }
      ImGui::SameLine(0, 14); caption("Start Key"); ImGui::SameLine();
      ImGui::SetNextItemWidth(86);
      if (ImGui::BeginCombo("##skey", noteName(startKey_))) {
        for (int k = 0; k < 120; ++k) if (ImGui::Selectable(noteName(k), k == startKey_)) { startKey_ = k; samplerRemapKeys(s); }
        ImGui::EndCombo();
      }
      ImGui::SameLine(0, 14);
      int chrom = chromatic_ ? 0 : 1;
      ImGui::SetNextItemWidth(110);
      if (ImGui::Combo("##chrom", &chrom, "Chromatic\0White keys\0")) { chromatic_ = chrom == 0; samplerRemapKeys(s); }
      ImGui::SameLine(0, 14);
      if (st::chip("Gate", !playToEnd_, gold, ImVec2(54, 0), goldText)) { playToEnd_ = false; samplerRemapKeys(s); }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("A slice plays while the key is held");
      ImGui::SameLine(0, 2);
      if (st::chip("Play to End", playToEnd_, gold, ImVec2(86, 0), goldText)) { playToEnd_ = true; samplerRemapKeys(s); }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("A slice plays to its end even after the key is released");
      ImGui::SameLine(0, 14);
      ImGui::BeginDisabled();
      st::chip("Follow Tempo", false, gold, ImVec2(104, 0));
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Needs time-stretching, which is not built yet.\nUse the pitch controls or the tempo-synced LFOs in the meantime.");
    } else if (!zones.empty()) {
      Zone z = zones[static_cast<std::size_t>(std::clamp(selZone_, 0, static_cast<int>(zones.size()) - 1))];
      bool zc = false;
      caption("Loop"); ImGui::SameLine();
      int lm = static_cast<int>(z.loop);
      ImGui::SetNextItemWidth(110);
      if (ImGui::Combo("##loop", &lm, "Off\0Forward\0Ping-pong\0")) { z.loop = static_cast<LoopMode>(lm); if (z.loop != LoopMode::Off && z.loopEnd < 0) { z.loopStart = z.start; } zc = true; }
      ImGui::SameLine(0, 14); caption("Crossfade"); ImGui::SameLine();
      int xfd = static_cast<int>(z.loopCrossfade);
      ImGui::SetNextItemWidth(90);
      if (ImGui::DragInt("##xf", &xfd, 5.f, 0, 20000, "%d")) { z.loopCrossfade = xfd; zc = true; }
      ImGui::SameLine(0, 14); caption("Root"); ImGui::SameLine();
      ImGui::SetNextItemWidth(86);
      if (ImGui::BeginCombo("##root", noteName(z.rootKey))) {
        for (int k = 0; k < 128; ++k) if (ImGui::Selectable(noteName(k), k == z.rootKey)) { z.rootKey = k; zc = true; }
        ImGui::EndCombo();
      }
      ImGui::SameLine(0, 14);
      if (st::chip("Reverse", z.reverse, gold, ImVec2(70, 0), goldText)) { z.reverse = !z.reverse; zc = true; }
      ImGui::SameLine(0, 14); caption("Tune"); ImGui::SameLine();
      ImGui::SetNextItemWidth(80);
      if (ImGui::DragFloat("##zt", &z.tuneCents, 0.5f, -1200.f, 1200.f, "%.1f ct")) zc = true;
      if (zc) s->updateZone(static_cast<std::size_t>(std::clamp(selZone_, 0, static_cast<int>(zones.size()) - 1)), z);
    } else ImGui::Dummy(ImVec2(0, 24));
  }
  ImGui::Dummy(ImVec2(0, 2));

  // ============================ lower panels ================================================
  const float kbH = 56.f;
  const float panelsH = std::max(250.f, ImGui::GetContentRegionAvail().y - kbH - 36.f);
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float usable = fullW - gap * 3.f;
  const float wLfo = usable * 0.31f, wPitch = usable * 0.23f, wFilter = usable * 0.23f, wAmp = usable - wLfo - wPitch - wFilter;
  const ImU32 orange = 0xFF3D5EF0;

  // ---- LFO
  ImGui::BeginChild("##lfo", ImVec2(wLfo, panelsH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  {
    for (int k = 0; k < 2; ++k) {
      if (k) ImGui::SameLine(0, 2);
      ImGui::PushID(k);
      st::powerButton("##pw", &p.lfo[k].on) ? pc = true : false;
      ImGui::SameLine(0, 2);
      char nm[12]; std::snprintf(nm, sizeof nm, "LFO %d", k + 1);
      if (st::chip(nm, lfoTab_ == k, 0xFFD2882F, ImVec2(56, 22))) lfoTab_ = k;
      ImGui::PopID();
    }
    ImGui::SameLine(0, 8);
    if (st::chip("MOD MATRIX", lfoTab_ == 2, 0xFFD2882F, ImVec2(98, 22))) lfoTab_ = 2;
    ImGui::Dummy(ImVec2(0, 4));
    if (lfoTab_ < 2) {
      LfoVia dummyVia = LfoVia::None; (void)dummyVia;
      const int k = lfoTab_;
      instruments::LfoParams& l = p.lfo[k];
      ImGui::PushID(100 + k);
      // knobs
      ImGui::BeginGroup();
      if (l.sync) pc |= st::knob("##rate", "Rate (beats)", &l.syncBeats, 0.0625f, 16.f, 1.f, "%.3g beats", gold, 44.f, false, true);
      else pc |= st::knob("##rate", "Rate", &l.rateHz, 0.05f, 30.f, 5.f, "%.2f Hz", gold, 44.f, false, true);
      ImGui::EndGroup();
      ImGui::SameLine(0, 2);
      {
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(sp.x, sp.y + 10.f));
        if (st::chip("Sync", l.sync, gold, ImVec2(40, 20), goldText)) { l.sync = !l.sync; pc = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock the rate to the tempo of the session");
        ImGui::SetCursorScreenPos(ImVec2(sp.x + 46.f, sp.y));
      }
      ImGui::SameLine(0, 10);
      pc |= st::knob("##fade", "Fade In", &l.fadeIn, 0.f, 5.f, 0.f, "%.2f s", gold);
      ImGui::SameLine(0, 10);
      { float deg = l.phase * 360.f; if (st::knob("##phase", "Phase", &deg, 0.f, 360.f, 0.f, "%.0f deg", gold)) { l.phase = deg / 360.f; pc = true; } }
      // menus
      caption("Waveform"); ImGui::SameLine(110); caption("Mode"); ImGui::SameLine(190); caption("Key Trigger");
      int shape = static_cast<int>(l.shape);
      ImGui::SetNextItemWidth(96);
      if (ImGui::Combo("##shape", &shape, "Sine\0Triangle\0Square\0Saw up\0Saw down\0")) { l.shape = static_cast<LfoShape>(shape); pc = true; }
      ImGui::SameLine(110);
      ImGui::SetNextItemWidth(66);
      int pm = l.poly ? 0 : 1;
      if (ImGui::Combo("##poly", &pm, "Poly\0Mono\0")) { l.poly = pm == 0; pc = true; }
      ImGui::SameLine(190);
      if (st::chip(l.keyTrigger ? "ON" : "OFF", l.keyTrigger, gold, ImVec2(48, 0), goldText)) { l.keyTrigger = !l.keyTrigger; pc = true; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("ON: the LFO restarts with every note (Poly mode).\nOFF: it runs freely.");
      int pol = l.bipolar ? 0 : 1;
      ImGui::SetNextItemWidth(96);
      if (ImGui::Combo("##pol", &pol, "Bipolar\0Unipolar\0")) { l.bipolar = pol == 0; pc = true; }
      ImGui::Dummy(ImVec2(0, 4));
      ImGui::Separator();
      // one route per LFO: amount, via, target
      int& tg = lfoTarget_[k];
      caption("Amount"); ImGui::SameLine(110); caption("Via"); ImGui::SameLine(200); caption("Target");
      float amount = tg == 0 ? l.pitchCents / 12.f : tg == 1 ? l.filterOctaves * 25.f : l.amp * 100.f;
      ImGui::SetNextItemWidth(96);
      if (ImGui::DragFloat("##amt", &amount, 0.5f, tg == 2 ? 0.f : -100.f, 100.f, "%+.1f %%")) {
        if (tg == 0) l.pitchCents = amount * 12.f; else if (tg == 1) l.filterOctaves = amount / 25.f; else l.amp = std::clamp(amount / 100.f, 0.f, 1.f);
        pc = true;
      }
      ImGui::SameLine(110);
      int via = static_cast<int>(l.via);
      ImGui::SetNextItemWidth(84);
      if (ImGui::Combo("##via", &via, "-\0Mod Wheel\0Velocity\0")) { l.via = static_cast<LfoVia>(via); pc = true; }
      ImGui::SameLine(200);
      ImGui::SetNextItemWidth(100);
      ImGui::Combo("##tgt", &tg, "Pitch\0Flt Cutoff\0Amp\0");
      ImGui::SetNextItemWidth(-1);
      if (ImGui::SliderFloat("##amts", &amount, tg == 2 ? 0.f : -100.f, 100.f, "")) {
        if (tg == 0) l.pitchCents = amount * 12.f; else if (tg == 1) l.filterOctaves = amount / 25.f; else l.amp = std::clamp(amount / 100.f, 0.f, 1.f);
        pc = true;
      }
      ImGui::PopID();
    } else {
      ImGui::TextDisabled("Depth of each LFO on each destination (100 %% = 12 semitones, 4 octaves, full level).");
      if (ImGui::BeginTable("##mm", 5, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(""); ImGui::TableSetupColumn("Pitch"); ImGui::TableSetupColumn("Cutoff"); ImGui::TableSetupColumn("Amp"); ImGui::TableSetupColumn("Via");
        ImGui::TableHeadersRow();
        for (int k = 0; k < 2; ++k) {
          instruments::LfoParams& l = p.lfo[k];
          ImGui::PushID(k);
          ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("LFO %d", k + 1);
          float a = l.pitchCents / 12.f, b = l.filterOctaves * 25.f, c2 = l.amp * 100.f;
          ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::DragFloat("##a", &a, 0.5f, -100.f, 100.f, "%+.0f%%")) { l.pitchCents = a * 12.f; pc = true; }
          ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::DragFloat("##b", &b, 0.5f, -100.f, 100.f, "%+.0f%%")) { l.filterOctaves = b / 25.f; pc = true; }
          ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::DragFloat("##c", &c2, 0.5f, 0.f, 100.f, "%.0f%%")) { l.amp = c2 / 100.f; pc = true; }
          ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); int via = static_cast<int>(l.via); if (ImGui::Combo("##v", &via, "-\0Mod Wheel\0Velocity\0")) { l.via = static_cast<LfoVia>(via); pc = true; }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // ---- PITCH
  ImGui::BeginChild("##pitch", ImVec2(wPitch, panelsH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  {
    panelTitle("PITCH");
    pc |= st::knob("##coarse", "Coarse", &p.coarseSemitones, -24.f, 24.f, 0.f, "%+.0f st", gold, 44.f, true);
    ImGui::SameLine(0, 8);
    pc |= st::knob("##fine", "Fine", &p.masterTuneCents, -100.f, 100.f, 0.f, "%+.0f ct", gold, 44.f, true);
    ImGui::SameLine(0, 8);
    pc |= st::knob("##glide", "Glide", &p.glideSeconds, 0.f, 2.f, 0.f, "%.2f s", gold);
    pc |= st::knob("##penv", "Env Depth", &p.pitchEnvCents, -2400.f, 2400.f, 0.f, "%+.0f ct", gold, 44.f, true);
    ImGui::SameLine(0, 8);
    ImGui::BeginGroup();
    caption("Key Tracking");
    if (st::chip(p.pitchKeyTrack ? "ON" : "OFF", p.pitchKeyTrack, gold, ImVec2(48, 0), goldText)) { p.pitchKeyTrack = !p.pitchKeyTrack; pc = true; }
    caption("Bend Range");
    {
      static const float ranges[] = {0.f, 1.f, 2.f, 3.f, 5.f, 7.f, 12.f, 24.f};
      char cur[16]; std::snprintf(cur, sizeof cur, "%.0f", static_cast<double>(p.pitchBendSemitones));
      ImGui::SetNextItemWidth(58);
      if (ImGui::BeginCombo("##bend", cur)) {
        for (float r : ranges) { char b[8]; std::snprintf(b, sizeof b, "%.0f", static_cast<double>(r)); if (ImGui::Selectable(b, r == p.pitchBendSemitones)) { p.pitchBendSemitones = r; pc = true; } }
        ImGui::EndCombo();
      }
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 4));
    const float gw = ImGui::GetContentRegionAvail().x - 22.f;
    ImGui::BeginGroup();
    pc |= st::envelopeGraph("penv", &p.pitchEnv.attack, &p.pitchEnv.hold, &p.pitchEnv.decay, &p.pitchEnv.sustain, &p.pitchEnv.release, gw, 84.f);
    caption("AHDSR");
    ImGui::EndGroup();
    ImGui::SameLine(0, 4);
    ImGui::BeginGroup();
    pc |= st::thinVSlider("##pvel", &p.pitchEnvVelocity, 0.f, 1.f, 84.f);
    caption("Vel");
    ImGui::EndGroup();
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // ---- FILTER
  ImGui::BeginChild("##filter", ImVec2(wFilter, panelsH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  {
    bool fon = p.filter != FilterType::Off;
    if (st::powerButton("##fpw", &fon)) { p.filter = fon ? FilterType::LowPass : FilterType::Off; pc = true; }
    ImGui::SameLine(0, 4);
    panelTitle("FILTER");
    ImGui::SameLine(0, 10);
    int ft = std::max(0, static_cast<int>(p.filter) - 1);
    ImGui::BeginDisabled(!fon);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##ftype", &ft, "LP 12 dB\0HP 12 dB\0BP 12 dB\0")) { p.filter = static_cast<FilterType>(ft + 1); pc = true; }
    ImGui::EndDisabled();
    pc |= st::knob("##cut", "Cutoff", &p.cutoffHz, 20.f, 20000.f, 20000.f, "%.0f Hz", orange, 56.f, false, true);
    ImGui::SameLine(0, 6);
    pc |= st::knob("##res", "Reso", &p.resonance, 0.f, 1.f, 0.f, "%.2f");
    ImGui::SameLine(0, 6);
    pc |= st::knob("##drv", "Drive", &p.filterDrive, 0.f, 1.f, 0.f, "%.2f");
    pc |= st::knob("##fenv", "Env Depth", &p.filterEnvOctaves, -8.f, 8.f, 0.f, "%+.1f oct", gold, 44.f, true);
    ImGui::SameLine(0, 8);
    pc |= st::knob("##fkey", "Keyscale", &p.filterKeyTrack, 0.f, 1.f, 0.f, "%.2f");
    ImGui::Dummy(ImVec2(0, 2));
    const float gw = ImGui::GetContentRegionAvail().x - 22.f;
    ImGui::BeginGroup();
    pc |= st::envelopeGraph("fenv", &p.filterEnv.attack, &p.filterEnv.hold, &p.filterEnv.decay, &p.filterEnv.sustain, &p.filterEnv.release, gw, 70.f);
    caption("AHDSR");
    ImGui::EndGroup();
    ImGui::SameLine(0, 4);
    ImGui::BeginGroup();
    { float v = p.filterVelOctaves; if (st::thinVSlider("##fvel", &v, 0.f, 6.f, 70.f)) { p.filterVelOctaves = v; pc = true; } }
    caption("Vel");
    ImGui::EndGroup();
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // ---- AMP
  ImGui::BeginChild("##amp", ImVec2(0, panelsH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  {
    panelTitle("AMP");
    pc |= st::knob("##pan", "Pan", &p.pan, -1.f, 1.f, 0.f, "%+.2f", gold, 44.f, true);
    ImGui::SameLine(0, 8);
    ImGui::BeginGroup();
    caption("Polyphony");
    {
      static const int counts[] = {1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64};
      char cur[8]; std::snprintf(cur, sizeof cur, "%d", p.polyphony);
      ImGui::SetNextItemWidth(62);
      if (ImGui::BeginCombo("##poly", cur)) {
        for (int c : counts) { char b[8]; std::snprintf(b, sizeof b, "%d", c); if (ImGui::Selectable(b, c == p.polyphony)) { p.polyphony = c; pc = true; } }
        ImGui::EndCombo();
      }
    }
    if (st::chip("Mono", p.mono, gold, ImVec2(48, 0), goldText)) { p.mono = !p.mono; pc = true; }
    ImGui::SameLine(0, 2);
    if (st::chip("Legato", p.legato, gold, ImVec2(54, 0), goldText)) { p.legato = !p.legato; pc = true; }
    ImGui::EndGroup();
    pc |= st::knob("##vol", "Volume", &p.masterGainDb, -60.f, 12.f, 0.f, "%.1f dB", gold, 64.f);
    ImGui::Dummy(ImVec2(0, 2));
    const float gw = ImGui::GetContentRegionAvail().x - 52.f;
    const float gtop = ImGui::GetCursorScreenPos().y;
    ImGui::BeginGroup();
    pc |= st::envelopeGraph("aenv", &p.amp.attack, &p.amp.hold, &p.amp.decay, &p.amp.sustain, &p.amp.release, gw, 70.f);
    caption("AHDSR");
    ImGui::EndGroup();
    ImGui::SameLine(0, 4);
    ImGui::BeginGroup();
    pc |= st::thinVSlider("##avel", &p.velocityToAmp, 0.f, 1.f, 70.f);
    caption("Vel");
    ImGui::EndGroup();
    ImGui::SameLine(0, 6);
    {
      const ImVec2 mp = ImGui::GetCursorScreenPos();
      const session::Track* tr = session_->track(selTrack_);
      float l = 0.f, r = 0.f;
      if (tr) { const auto it = levels_.find(tr->strip); if (it != levels_.end()) { l = it->second[0]; r = it->second[1]; } }
      st::ledMeter(ImGui::GetWindowDrawList(), ImVec2(mp.x, gtop), ImVec2(8.f, 70.f), l, false);
      st::ledMeter(ImGui::GetWindowDrawList(), ImVec2(mp.x + 10.f, gtop), ImVec2(8.f, 70.f), r, false);
      ImGui::Dummy(ImVec2(20.f, 70.f));
    }
  }
  ImGui::EndChild();

  // ============================ keyboard ====================================================
  {
    const ImVec2 kp = ImGui::GetCursorScreenPos();
    const float keyW = fullW / 128.f;
    int hover = -1;
    const ImVec2 ms = ImGui::GetIO().MousePos;
    for (int pass = 0; pass < 2; ++pass)
      for (int k = 0; k < 128; ++k) {
        const bool black = isBlack(k);
        if ((pass == 0) == black) continue;
        const ImVec2 a(kp.x + static_cast<float>(k) * keyW, kp.y), b(kp.x + static_cast<float>(k + 1) * keyW, kp.y + (black ? kbH * 0.62f : kbH));
        bool inZone = false;
        for (const Zone& z : zones) if (k >= z.keyLo && k <= z.keyHi) { inZone = true; break; }
        ImU32 c = k == heldKey_ ? gold : black ? (inZone ? st::rgba(0.16f, 0.30f, 0.50f) : st::rgba(0.07f, 0.07f, 0.08f)) : (inZone ? st::rgba(0.80f, 0.88f, 1.f) : st::rgba(0.60f, 0.61f, 0.64f));
        dl->AddRectFilled(a, b, c);
        dl->AddRect(a, b, st::rgba(0.f, 0.f, 0.f, 0.5f));
        if (ms.x >= a.x && ms.x < b.x && ms.y >= a.y && ms.y < b.y && (pass == 1 || !black)) hover = k;
      }
    ImGui::InvisibleButton("##kb", ImVec2(fullW, kbH));
    if (ImGui::IsItemActive() && hover >= 0) {
      if (hover != heldKey_) { if (heldKey_ >= 0) s->noteOff(heldKey_); s->noteOn(hover, static_cast<int>(keyboardVelocity_)); heldKey_ = hover; }
    } else if (heldKey_ >= 0) { s->noteOff(heldKey_); heldKey_ = -1; }
    ImGui::AlignTextToFramePadding(); caption("Velocity"); ImGui::SameLine();
    ImGui::SetNextItemWidth(140); ImGui::SliderFloat("##kv", &keyboardVelocity_, 1.f, 127.f, "%.0f");
    ImGui::SameLine(); caption("voices:"); ImGui::SameLine(); ImGui::Text("%d", s->activeVoices());
  }
  if (pc) s->setParams(p);
  ImGui::End();
  if (showZones_) drawZoneWindow(s);
}

}  // namespace wezt::ui
