// Theme and the demo session.
#include <cmath>
#include <filesystem>
#include <random>

#include <imgui.h>

#include "io/wav.h"
#include "ui/app.h"

#include "ui/font_roboto.inc"

namespace wezt::ui {
namespace fs = std::filesystem;

void App::loadFonts(float scale) {
  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig fc;
  fc.OversampleH = 2; fc.OversampleV = 1;
  io.Fonts->AddFontFromMemoryCompressedBase85TTF(RobotoMedium_compressed_data_base85, 15.f * scale, &fc);
}

void App::applyTheme() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 2.f; s.ChildRounding = 2.f; s.FrameRounding = 2.f; s.GrabRounding = 2.f; s.PopupRounding = 3.f; s.TabRounding = 2.f; s.ScrollbarRounding = 4.f;
  s.FramePadding = ImVec2(7.f, 4.f); s.ItemSpacing = ImVec2(6.f, 5.f); s.WindowPadding = ImVec2(8.f, 6.f);
  s.ScrollbarSize = 12.f; s.WindowBorderSize = 1.f; s.ChildBorderSize = 1.f; s.FrameBorderSize = 0.f;
  ImVec4* c = s.Colors;
  // graphite panels, warm-white text, one azure accent
  const ImVec4 bg(0.105f, 0.108f, 0.115f, 1.f), panel(0.150f, 0.153f, 0.165f, 1.f), item(0.235f, 0.240f, 0.260f, 1.f), accent(0.22f, 0.50f, 0.82f, 1.f);
  c[ImGuiCol_WindowBg] = bg; c[ImGuiCol_ChildBg] = panel; c[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.135f, 0.145f, 0.98f);
  c[ImGuiCol_Border] = ImVec4(0.05f, 0.05f, 0.06f, 1.f); c[ImGuiCol_Text] = ImVec4(0.89f, 0.90f, 0.92f, 1.f); c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.57f, 0.61f, 1.f);
  c[ImGuiCol_FrameBg] = ImVec4(0.07f, 0.072f, 0.08f, 1.f); c[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.125f, 0.14f, 1.f); c[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.16f, 0.18f, 1.f);
  c[ImGuiCol_Button] = item; c[ImGuiCol_ButtonHovered] = ImVec4(0.31f, 0.32f, 0.35f, 1.f); c[ImGuiCol_ButtonActive] = accent;
  c[ImGuiCol_Header] = ImVec4(0.22f, 0.50f, 0.82f, 0.45f); c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.50f, 0.82f, 0.65f); c[ImGuiCol_HeaderActive] = accent;
  c[ImGuiCol_SliderGrab] = ImVec4(0.55f, 0.72f, 0.92f, 1.f); c[ImGuiCol_SliderGrabActive] = ImVec4(0.72f, 0.86f, 1.f, 1.f); c[ImGuiCol_CheckMark] = ImVec4(0.62f, 0.80f, 1.f, 1.f);
  c[ImGuiCol_Tab] = item; c[ImGuiCol_TabHovered] = accent; c[ImGuiCol_TabActive] = ImVec4(0.20f, 0.44f, 0.72f, 1.f);
  c[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.07f, 0.08f, 1.f); c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.20f, 0.30f, 1.f);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.085f, 0.087f, 0.095f, 1.f); c[ImGuiCol_TableRowBgAlt] = ImVec4(1.f, 1.f, 1.f, 0.03f); c[ImGuiCol_TableHeaderBg] = item;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.06f, 0.07f, 1.f); c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.29f, 0.32f, 1.f);
  c[ImGuiCol_Separator] = ImVec4(0.05f, 0.05f, 0.06f, 1.f);
}

void App::loadDemo() {
  const double rate = session_->sampleRate();
  const fs::path dir = fs::temp_directory_path() / "wezt_demo";
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> noise(-1.f, 1.f);
  auto make = [&](double seconds) { return AudioBuffer(1, static_cast<std::size_t>(seconds * rate)); };
  const double pi = 3.14159265358979;
  // drums: kick on every beat, hats on the off-beats (120 bpm)
  AudioBuffer drums = make(6.0);
  for (int beat = 0; beat < 12; ++beat) {
    const std::size_t at = static_cast<std::size_t>(beat * 0.5 * rate);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.35 * rate) && at + i < drums.frames; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double f = 45.0 + 90.0 * std::exp(-t * 28.0);
      drums.channel(0)[at + i] += static_cast<float>(0.9 * std::sin(2 * pi * f * t) * std::exp(-t * 9.0));
    }
    const std::size_t hat = at + static_cast<std::size_t>(0.25 * rate);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.05 * rate) && hat + i < drums.frames; ++i)
      drums.channel(0)[hat + i] += 0.25f * noise(rng) * std::exp(-static_cast<float>(i) / static_cast<float>(0.012 * rate));
  }
  AudioBuffer bass = make(6.0);
  const double notes[] = {55.0, 55.0, 73.4, 65.4};
  for (int n = 0; n < 12; ++n) {
    const double f = notes[(n / 3) % 4];
    const std::size_t at = static_cast<std::size_t>(n * 0.5 * rate);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.48 * rate) && at + i < bass.frames; ++i) {
      const double t = static_cast<double>(i) / rate;
      double v = 0.0;
      for (int h = 1; h <= 6; ++h) v += std::sin(2 * pi * f * h * t) / h;
      bass.channel(0)[at + i] = static_cast<float>(0.35 * v * std::min(1.0, t * 200.0) * std::exp(-t * 2.5));
    }
  }
  AudioBuffer pad = make(5.0);
  for (std::size_t i = 0; i < pad.frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    double v = 0.0;
    for (double f : {220.0, 277.2, 329.6, 440.3}) v += std::sin(2 * pi * f * t) + std::sin(2 * pi * (f * 1.004) * t);
    const double env = std::min(1.0, t / 1.2) * std::min(1.0, (5.0 - t) / 1.5);
    pad.channel(0)[i] = static_cast<float>(0.07 * v * env);
  }
  AudioBuffer fx = make(2.5);
  for (std::size_t i = 0; i < fx.frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    fx.channel(0)[i] = static_cast<float>(0.3 * noise(rng) * std::sin(pi * t / 2.5) * std::sin(2 * pi * (200.0 + 600.0 * t) * t));
  }
  auto put = [&](const char* name, const AudioBuffer& b) { const std::string p = (dir / name).string(); io::writeWavFloat(p, b, static_cast<unsigned>(rate)); return p; };
  const std::string pd = put("drums.wav", drums), pb = put("bass.wav", bass), pp = put("pad.wav", pad), pf = put("sweep.wav", fx);

  session::TrackId d = selTrack_;
  session_->track(d)->name = "Drums";
  const session::TrackId b = session_->addTrack("Bass", true), p = session_->addTrack("Pad", true), f = session_->addTrack("Sweep FX", true), k = session_->addTrack("Keys", true);
  session_->addClipFromFile(d, pd, 0);
  session_->addClipFromFile(b, pb, 0);
  session_->addClipFromFile(p, pp, static_cast<std::int64_t>(rate));
  session_->addClipFromFile(f, pf, static_cast<std::int64_t>(2.5 * rate));
  // keys: a sampler holding eight drum hits, sliced onto the keys from C2
  auto sampler = std::make_unique<instruments::Sampler>();
  auto hits = std::make_shared<AudioBuffer>(2, static_cast<std::size_t>(2.6 * rate));
  for (int i = 0; i < 8; ++i) {
    const std::size_t at = static_cast<std::size_t>((0.05 + 0.31 * i) * rate);
    const double f0 = 170.0 + 55.0 * (i % 3), len = (i % 4 == 3) ? 0.28 : 0.2;
    for (std::size_t n = 0; n < static_cast<std::size_t>(len * rate) && at + n < hits->frames; ++n) {
      const double t = static_cast<double>(n) / rate;
      const double env = std::exp(-t * (i % 2 ? 16.0 : 24.0)) * std::min(1.0, t * 800.0);
      const float nz = noise(rng) * static_cast<float>(std::exp(-t * 90.0));
      const float amp = 0.55f + 0.1f * static_cast<float>(i % 3);
      hits->channel(0)[at + n] += amp * (static_cast<float>(std::sin(2 * pi * f0 * t * (1.0 + 0.6 * std::exp(-t * 40.0))) * env) + 0.4f * nz);
      hits->channel(1)[at + n] += amp * (static_cast<float>(std::sin(2 * pi * f0 * 1.012 * t * (1.0 + 0.6 * std::exp(-t * 40.0))) * env) * 0.9f + 0.4f * noise(rng) * static_cast<float>(std::exp(-t * 90.0)));
    }
  }
  sampler->sliceTransients(hits, rate, 0.8f, 64, 48);
  sliceSource_ = hits; sliceRate_ = rate; startKey_ = 48;
  instruments::SamplerParams sp;
  sp.mode = instruments::SamplerMode::Slice;
  sp.amp = {0.002f, 0.3f, 0.9f, 0.15f};
  sp.filter = instruments::FilterType::LowPass; sp.cutoffHz = 9000.f; sp.resonance = 0.2f; sp.filterEnvOctaves = 1.5f;
  sp.filterEnv = {0.005f, 0.25f, 0.4f, 0.3f};
  sp.lfo[0].rateHz = 5.f; sp.lfo[0].pitchCents = 8.f; sp.lfo[0].fadeIn = 0.4f;
  sampler->setParams(sp);
  session_->addInsert(k, std::move(sampler), instruments::Sampler::descriptor(), 0);
  session_->addBus("Reverb");
  session_->addSend(p, 1, -8.f, false);
  session_->track(b)->strip->setGainDb(-4.f);
  session_->track(p)->strip->setPan(0.15f);
  session_->track(f)->strip->setGainDb(-7.f);
  session_->track(k)->strip->setGainDb(-6.f);
  selTrack_ = d;
  objects_[d] = {0.f, 0.85f, 0.f, 0.f};
  objects_[b] = {0.f, 0.15f, 0.f, 0.3f};
  objects_[p] = {-0.55f, -0.3f, 0.75f, 0.6f};
  objects_[f] = {0.7f, 0.5f, 0.35f, 0.1f};
  objects_[k] = {-0.8f, 0.4f, 0.1f, 0.f};
  zoomTo(0, static_cast<std::int64_t>(7.0 * rate));
  sessionName_ = "Sunset at the Border";
  markers_.clear(); groups_.clear(); hidden_.clear(); pinned_.clear(); firstTrack_ = 0;
  addMarker(0, "intro");
  addMarker(static_cast<std::int64_t>(2.0 * rate), "verse");
  addMarker(static_cast<std::int64_t>(4.5 * rate), "chorus");
  addGroup("BAND", {d, b, k});
  groups_.back().active = false;
  setStatus("Demo session loaded");
}

}  // namespace wezt::ui
