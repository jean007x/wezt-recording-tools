// Renders one frame of the Wezt UI on the CPU and writes it as a PPM image. Used to check the layout
// without a window or a GPU, and as a regression aid. Usage: wezt_ui_snapshot out.ppm [edit|mix|sampler|immersive|assistant]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include "dsp/effect_plugins.h"
#include "instruments/builtin.h"
#include "ui/app.h"
#include "ui/intro.h"
#include "ui/logo.h"

namespace {
struct Image { int w, h; std::vector<float> rgb; };
// Pictures the intro draws (sentinel texture ids); every other draw command samples the font atlas.
struct Pic { int w = 0, h = 0; std::vector<unsigned char> rgba; };
Pic g_gold, g_white;
constexpr std::intptr_t kGoldId = 0x1090, kWhiteId = 0x1091;

void rasterize(const ImDrawData* dd, const unsigned char* fontTex, int fontW, int fontH, Image& img) {
  for (int n = 0; n < dd->CmdListsCount; ++n) {
    const ImDrawList* dl = dd->CmdLists[n];
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
      if (cmd.UserCallback) continue;
      const std::intptr_t id = (std::intptr_t)cmd.GetTexID();
      const unsigned char* tex = id == kGoldId ? g_gold.rgba.data() : id == kWhiteId ? g_white.rgba.data() : fontTex;
      const int tw = id == kGoldId ? g_gold.w : id == kWhiteId ? g_white.w : fontW, th = id == kGoldId ? g_gold.h : id == kWhiteId ? g_white.h : fontH;
      const int cx0 = std::max(0, static_cast<int>(cmd.ClipRect.x)), cy0 = std::max(0, static_cast<int>(cmd.ClipRect.y));
      const int cx1 = std::min(img.w, static_cast<int>(std::ceil(cmd.ClipRect.z))), cy1 = std::min(img.h, static_cast<int>(std::ceil(cmd.ClipRect.w)));
      for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
        const ImDrawVert* v[3];
        for (int k = 0; k < 3; ++k) v[k] = &dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + k]];
        const float minx = std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}), maxx = std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x});
        const float miny = std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}), maxy = std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y});
        const int x0 = std::max(cx0, static_cast<int>(std::floor(minx))), x1 = std::min(cx1, static_cast<int>(std::ceil(maxx)));
        const int y0 = std::max(cy0, static_cast<int>(std::floor(miny))), y1 = std::min(cy1, static_cast<int>(std::ceil(maxy)));
        const float den = (v[1]->pos.y - v[2]->pos.y) * (v[0]->pos.x - v[2]->pos.x) + (v[2]->pos.x - v[1]->pos.x) * (v[0]->pos.y - v[2]->pos.y);
        if (std::fabs(den) < 1e-9f) continue;
        for (int y = y0; y < y1; ++y)
          for (int x = x0; x < x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float a = ((v[1]->pos.y - v[2]->pos.y) * (px - v[2]->pos.x) + (v[2]->pos.x - v[1]->pos.x) * (py - v[2]->pos.y)) / den;
            const float b = ((v[2]->pos.y - v[0]->pos.y) * (px - v[2]->pos.x) + (v[0]->pos.x - v[2]->pos.x) * (py - v[2]->pos.y)) / den;
            const float c = 1.f - a - b;
            if (a < -1e-4f || b < -1e-4f || c < -1e-4f) continue;
            const float u = a * v[0]->uv.x + b * v[1]->uv.x + c * v[2]->uv.x, vv = a * v[0]->uv.y + b * v[1]->uv.y + c * v[2]->uv.y;
            const int tx = std::clamp(static_cast<int>(u * static_cast<float>(tw)), 0, tw - 1), ty = std::clamp(static_cast<int>(vv * static_cast<float>(th)), 0, th - 1);
            const unsigned char* t = tex + 4 * (ty * tw + tx);
            float col[4];
            for (int k = 0; k < 4; ++k) {
              const auto sh = static_cast<unsigned>(8 * k);
              const float vc = static_cast<float>((v[0]->col >> sh) & 255u) * a + static_cast<float>((v[1]->col >> sh) & 255u) * b + static_cast<float>((v[2]->col >> sh) & 255u) * c;
              col[k] = vc / 255.f * static_cast<float>(t[k]) / 255.f;
            }
            float* d = &img.rgb[static_cast<std::size_t>(y * img.w + x) * 3];
            for (int k = 0; k < 3; ++k) d[k] = col[k] * col[3] + d[k] * (1.f - col[3]);
          }
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "wezt_ui.ppm";
  const std::string view = argc > 2 ? argv[2] : "edit";
  const int W = 1600, H = 940;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(static_cast<float>(W), static_cast<float>(H));
  wezt::ui::App::loadFonts(1.f);
  unsigned char* tex = nullptr;
  int tw = 0, th = 0;
  io.Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);
  wezt::ui::App::applyTheme();
  if (view.rfind("intro", 0) == 0) {   // "intro3.8": the opening animation 3.8 s in, on its own (no studio behind it)
    const auto g = wezt::ui::makeGoldLogo(), w = wezt::ui::makeWhiteLogo();
    g_gold = {g.w, g.h, g.rgba}; g_white = {w.w, w.h, w.rgba};
    wezt::ui::Intro intro;
    intro.setTextures((ImTextureID)kGoldId, (ImTextureID)kWhiteId, static_cast<float>(g.w) / static_cast<float>(g.h));
    Image img{W, H, std::vector<float>(static_cast<std::size_t>(W * H) * 3, 0.f)};
    for (int frame = 0; frame < 2; ++frame) {
      io.DeltaTime = 1.f / 60.f;
      ImGui::NewFrame();
      if (frame == 1) {
        // the studio colour sits behind the flash once the intro has opened
        if (std::atof(view.c_str() + 5) >= wezt::ui::kIntroOpen) ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(W), static_cast<float>(H)), IM_COL32(23, 26, 31, 255));
        intro.paint(std::atof(view.c_str() + 5));
      }
      ImGui::Render();
      if (frame == 1) rasterize(ImGui::GetDrawData(), tex, tw, th, img);
    }
    FILE* f = std::fopen(out, "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out); return 1; }
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    std::vector<unsigned char> row(static_cast<std::size_t>(W) * 3);
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W * 3; ++x) row[static_cast<std::size_t>(x)] = static_cast<unsigned char>(std::clamp(img.rgb[static_cast<std::size_t>(y * W * 3 + x)], 0.f, 1.f) * 255.f + 0.5f);
      std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::printf("wrote %s (%s)\n", out, view.c_str());
    return 0;
  }
  wezt::ui::App app;
  app.loadDemo();
  // run a little audio so the meters and the playhead have something to show
  auto& eng = app.engine();
  eng.transport().locate(static_cast<std::int64_t>(48000 * 1.6));
  eng.transport().play();
  std::vector<float> l(512), r(512);
  float* op[2] = {l.data(), r.data()};
  for (int i = 0; i < 30; ++i) eng.process(nullptr, op, 512);
  eng.transport().stop();
  if (view == "mix") app.setView(wezt::ui::MainView::Mix);
  if (view == "sampler") { app.setView(wezt::ui::MainView::Edit); app.selectTrack(app.session().trackIds().back()); app.showSampler(true); }
  if (view == "assistant") {
    app.setView(wezt::ui::MainView::Edit);
    app.setAssistantDir("/tmp/wezt_snapshot_assistant");
    app.showAssistant(true);
    app.showMemory(true);
    app.assistantSay("hello");
    app.assistantSay("what can you do");
  }
  if (view == "pianoroll" || view == "score") {
    app.setView(wezt::ui::MainView::Edit);
    const int part = app.newMidiPart("Melody", "keys");
    app.editMidiPart(part, "notes", [](wezt::edit::PianoRoll& r) {
      const int p[] = {67, 69, 71, 74, 72, 71, 69, 66, 67, 71, 74, 79, 78, 76, 74, 72};
      const double st[] = {0, 1, 2, 3, 4, 5, 6, 6.5, 8, 9, 10, 11, 12, 13.5, 14, 15};
      const double ln[] = {1, 1, 1, 1, 1, 1, 0.5, 1.5, 1, 1, 1, 1, 1.5, 0.5, 1, 1};
      for (int i = 0; i < 16; ++i) r.addNote(p[i], st[i], ln[i], 70 + (i * 7) % 50, 0.25);
      r.addNote(55, 0, 4, 80, 0.25); r.addNote(59, 0, 4, 80, 0.25);
    });
    app.selectMidiPart(part);
    app.pianoRoll().selectRect(4, 7, 66, 72, false);
    if (view == "pianoroll") app.showPianoRoll(true); else app.showScore(true);
  }
  if (view == "clips") {
    app.setView(wezt::ui::MainView::Edit);
    app.session().newPlaylist(app.selectedTrack(), "Take 2", true);
    app.showClips(true);
  }
  if (view == "plugparams" || view == "plugdraw") {   // an EQ with a drawn lane; "plugdraw" also clicks in the graph
    app.setView(wezt::ui::MainView::Edit);
    const auto t = app.session().trackIds().front();
    wezt::plugins::PluginDescriptor d; d.name = "Wezt EQ"; d.format = wezt::plugins::Format::Builtin; d.bitness = 64;
    app.session().addInsert(t, wezt::instruments::makeBuiltin(d), d);
    const auto ps = app.session().track(t)->inserts[0].instance->parameters();
    const unsigned pid = ps.empty() ? 0 : ps.back().id;
    app.session().setParamAutomation(t, 0, pid, {{0, 0.2f}, {96000, 0.8f}, {192000, 0.4f}});
    app.selectPluginParam(t, 0, static_cast<int>(pid));
    app.showPluginParams(true);
  }
  if (view == "settings") { app.setView(wezt::ui::MainView::Edit); app.showSettings(true); }
  if (view == "immersive") { app.setView(wezt::ui::MainView::Edit); app.showImmersive(true); }
  app.setRange(static_cast<std::int64_t>(48000 * 1.0), static_cast<std::int64_t>(48000 * 2.5));
  Image img{W, H, std::vector<float>(static_cast<std::size_t>(W * H) * 3, 0.f)};
  const int clickX = argc > 3 ? std::atoi(argv[3]) : -1, clickY = argc > 4 ? std::atoi(argv[4]) : -1;   // optional: a mouse click, to try the drawing
  for (int frame = 0; frame < 6; ++frame) {   // the first frames settle window sizes
    io.DeltaTime = 1.f / 60.f;
    if (clickX >= 0) {
      io.AddMousePosEvent(static_cast<float>(clickX), static_cast<float>(clickY));
      if (frame == 3) io.AddMouseButtonEvent(0, true);
      if (frame == 4) io.AddMouseButtonEvent(0, false);
    }
    ImGui::NewFrame();
    app.frame();
    ImGui::Render();
    if (frame == 5) rasterize(ImGui::GetDrawData(), tex, tw, th, img);
  }
  FILE* f = std::fopen(out, "wb");
  if (!f) { std::fprintf(stderr, "cannot write %s\n", out); return 1; }
  std::fprintf(f, "P6\n%d %d\n255\n", W, H);
  std::vector<unsigned char> row(static_cast<std::size_t>(W) * 3);
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W * 3; ++x) row[static_cast<std::size_t>(x)] = static_cast<unsigned char>(std::clamp(img.rgb[static_cast<std::size_t>(y * W * 3 + x)], 0.f, 1.f) * 255.f + 0.5f);
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  std::printf("wrote %s (%s)\n", out, view.c_str());
  ImGui::DestroyContext();
  return 0;
}
