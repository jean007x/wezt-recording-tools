#include "ui/intro.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ui/intro_sound.h"

namespace wezt::ui {
namespace {

constexpr float kPi = 3.14159265f;

float clamp01(float x) { return std::clamp(x, 0.f, 1.f); }
float smooth(float a, float b, float x) { const float t = clamp01((x - a) / (b - a)); return t * t * (3.f - 2.f * t); }
ImU32 gold(float a, float bright = 1.f) {
  auto c = [&](float v) { return static_cast<int>(clamp01(v) * 255.f + 0.5f); };
  return IM_COL32(c(0.96f * bright), c(0.76f * bright), c(0.30f * bright), c(a));
}
float hash(std::uint32_t n) { n ^= n >> 16; n *= 0x7feb352dU; n ^= n >> 15; n *= 0x846ca68bU; n ^= n >> 16; return static_cast<float>(n & 0xffffff) / 16777215.f; }

struct Pose { float scale, yaw, tilt, alpha; };

Pose poseAt(double t) {
  Pose p{0.f, 0.f, 0.f, 1.f};
  const float tf = static_cast<float>(t);
  if (t < kIntroGrowEnd) {   // coming out of the distance: slow growth, fast spin that settles facing front exactly on landing
    const float u = tf / static_cast<float>(kIntroGrowEnd);
    p.scale = 0.025f + 0.975f * std::pow(u, 2.4f);
    p.yaw = 3.f * 2.f * kPi * (1.f - std::pow(1.f - u, 2.2f));
    p.tilt = 0.20f * std::sin(2.f * kPi * u * 1.5f) * (1.f - u);
    p.alpha = smooth(0.f, 0.3f, tf);
  } else if (t < kIntroHoldEnd) {   // landed: one small recoil, a slow sway that dies out
    const float s = tf - static_cast<float>(kIntroGrowEnd);
    p.scale = 1.f + 0.06f * std::exp(-s * 8.f) * std::cos(s * 18.f);
    p.yaw = 0.07f * std::sin(s * 3.2f) * std::exp(-s * 0.6f);
  } else if (t < kIntroOpen) {   // spinning back down to a speck, faster and faster
    const float v = (tf - static_cast<float>(kIntroHoldEnd)) / static_cast<float>(kIntroOpen - kIntroHoldEnd);
    p.scale = 1.f - 0.975f * std::pow(v, 2.2f);
    p.yaw = 2.f * 2.f * kPi * v * v + 0.07f * std::sin(0.7f * 3.2f) * std::exp(-0.7f * 0.6f) * (1.f - v);
    p.tilt = -0.15f * std::sin(kPi * v);
  } else p.alpha = 0.f;
  return p;
}

}  // namespace

void Intro::skip() {
  if (!active_ || t_ >= kIntroOpen) return;
  t_ = kIntroOpen;
  skipped_ = true;
}

void Intro::draw() {
  if (!active_) return;
  ImGuiIO& io = ImGui::GetIO();
  if (t_ < kIntroOpen) {
    bool key = ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1);
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart && !key; ++k) key = ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false);
    if (key && t_ > 0.15) skip();
  }
  t_ += std::min(0.05, static_cast<double>(io.DeltaTime));
  if (t_ >= kIntroEnd) { active_ = false; return; }
  paint(t_);
}

void Intro::paint(double t) const {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const ImVec2 c(size.x * 0.5f, size.y * 0.5f);
  const float tf = static_cast<float>(t);
  const float W0 = std::min(size.x * 0.73f, size.y * 0.68f * aspect_), H0 = W0 / aspect_;
  const float d = 1.6f * W0;

  if (t < kIntroOpen) {
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(5, 5, 8, 255));
    const Pose p = poseAt(t);
    const float u = clamp01(tf / static_cast<float>(kIntroGrowEnd));

    // a gold glow behind the logo that grows with it (stacked translucent discs)
    const float glow = std::pow(p.scale, 0.8f) * (t < kIntroGrowEnd ? 0.5f + 0.5f * u : 1.f) * p.alpha;
    for (int k = 0; k < 64; ++k) {   // many faint steps so the glow shades smoothly instead of showing rings
      const float r = (0.10f + 1.0f * static_cast<float>(k) / 63.f) * W0 * std::max(0.05f, p.scale);
      dl->AddCircleFilled(c, r, IM_COL32(235, 125, 30, static_cast<int>(clamp01(0.0075f * glow) * 255.f + 0.5f)), 72);
    }
    // dust drifting outward while it approaches
    if (t < kIntroGrowEnd)
      for (std::uint32_t i = 0; i < 70; ++i) {
        const float a = hash(i * 3 + 1) * 2.f * kPi, rr = (0.08f + hash(i * 3 + 2)) * W0 * 0.75f * std::pow(u, 1.2f);
        const float al = 0.55f * u * (0.3f + 0.7f * hash(i * 3 + 3)) * (1.f - smooth(0.9f, 1.f, u));
        dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * rr * 1.5f, c.y + std::sin(a) * rr * 0.8f), 0.8f + 1.6f * hash(i * 7 + 5), gold(al, 1.1f), 8);
      }
    // impact: shock rings, a horizontal flare, sparks
    if (t >= kIntroGrowEnd && t < kIntroGrowEnd + 1.1) {
      const float s = (tf - static_cast<float>(kIntroGrowEnd)) / 1.1f;
      dl->AddCircle(c, W0 * 0.95f * std::pow(s, 0.55f), gold(0.85f * std::pow(1.f - s, 1.5f)), 96, 1.f + 5.f * (1.f - s));
      dl->AddCircle(c, W0 * 0.55f * std::pow(s, 0.45f), gold(0.55f * std::pow(1.f - s, 1.5f), 1.2f), 96, 1.f + 2.f * (1.f - s));
      const float fh = 1.f + 6.f * std::pow(1.f - s, 2.f), fa = std::pow(1.f - s, 1.6f);
      const ImU32 z = gold(0.f), m = gold(0.9f * fa, 1.3f);
      dl->AddRectFilledMultiColor(ImVec2(c.x - size.x * 0.5f, c.y - fh * 0.5f), ImVec2(c.x, c.y + fh * 0.5f), z, m, m, z);
      dl->AddRectFilledMultiColor(ImVec2(c.x, c.y - fh * 0.5f), ImVec2(c.x + size.x * 0.5f, c.y + fh * 0.5f), m, z, z, m);
      for (std::uint32_t i = 0; i < 90; ++i) {
        const float a = hash(i * 5 + 11) * 2.f * kPi, sp = (0.25f + 1.1f * hash(i * 5 + 12)) * W0;
        const float r0 = sp * std::pow(s, 0.5f), r1 = sp * std::pow(std::max(0.f, s - 0.06f), 0.5f);
        const float al = std::pow(1.f - s, 1.3f);
        dl->AddLine(ImVec2(c.x + std::cos(a) * r0, c.y + std::sin(a) * r0 * 0.7f), ImVec2(c.x + std::cos(a) * r1, c.y + std::sin(a) * r1 * 0.7f), gold(al, 1.3f), 1.6f);
      }
    }

    // the logo: 48 vertical strips, each projected in 3D (spin about the vertical axis, a little tilt, perspective)
    if (p.alpha > 0.002f && p.scale > 0.f && gold_) {
      const float cy = std::cos(p.yaw), sy = std::sin(p.yaw), cx = std::cos(p.tilt), sx = std::sin(p.tilt);
      auto proj = [&](float lx, float ly) {
        const float x = lx * W0 * p.scale, y = ly * H0 * p.scale;
        const float x1 = x * cy, z1 = -x * sy;
        const float y2 = y * cx - z1 * sx, z2 = y * sx + z1 * cx;
        const float k = d / std::max(0.2f * d, d + z2);
        return ImVec2(c.x + x1 * k, c.y + y2 * k);
      };
      const float face = 0.5f + 0.5f * std::pow(std::fabs(cy), 0.6f);   // turned edge-on it goes dark, like metal away from the light
      const int a = static_cast<int>(clamp01(p.alpha) * 255.f + 0.5f), v = static_cast<int>(clamp01(face) * 255.f + 0.5f);
      const ImU32 tint = IM_COL32(v, v, v, a);
      constexpr int N = 48;
      float sweep = -1.f;
      static const float sweeps[3][2] = {{1.6f, 0.9f}, {2.8f, 0.9f}, {3.95f, 0.8f}};
      for (const auto& sw : sweeps) if (tf >= sw[0] && tf < sw[0] + sw[1]) sweep = -0.25f + 1.5f * (tf - sw[0]) / sw[1];
      for (int s = 0; s < N; ++s) {
        const float u0 = static_cast<float>(s) / N, u1 = static_cast<float>(s + 1) / N;
        const ImVec2 q0 = proj(u0 - 0.5f, -0.5f), q1 = proj(u1 - 0.5f, -0.5f), q2 = proj(u1 - 0.5f, 0.5f), q3 = proj(u0 - 0.5f, 0.5f);
        dl->AddImageQuad(gold_, q0, q1, q2, q3, ImVec2(u0, 0), ImVec2(u1, 0), ImVec2(u1, 1), ImVec2(u0, 1), tint);
        if (sweep > -0.9f && white_) {
          const float dd = ((u0 + u1) * 0.5f - sweep) / 0.07f;
          const float g = std::exp(-dd * dd) * 0.85f * clamp01(p.alpha);
          if (g > 0.01f) dl->AddImageQuad(white_, q0, q1, q2, q3, ImVec2(u0, 0), ImVec2(u1, 0), ImVec2(u1, 1), ImVec2(u0, 1), IM_COL32(255, 245, 215, static_cast<int>(g * 255.f)));
        }
      }
    }
    // sparkle flares on the polished edges while it holds
    if (t >= kIntroGrowEnd + 0.1 && t < kIntroHoldEnd + 0.4 && gold_) {
      static const float spots[5][2] = {{-0.33f, -0.30f}, {0.20f, 0.26f}, {0.44f, -0.14f}, {-0.12f, 0.06f}, {-0.46f, 0.12f}};
      for (int k = 0; k < 5; ++k) {
        const float ph = (tf - static_cast<float>(kIntroGrowEnd) - 0.1f - 0.17f * static_cast<float>(k)) / 0.55f;
        if (ph <= 0.f || ph >= 1.f) continue;
        const float a = std::sin(ph * kPi), L = W0 * (0.015f + 0.060f * a);
        const ImVec2 q(c.x + spots[k][0] * W0, c.y + spots[k][1] * H0);
        dl->AddLine(ImVec2(q.x - L, q.y), ImVec2(q.x + L, q.y), IM_COL32(255, 244, 214, static_cast<int>(a * 235.f)), 1.5f);
        dl->AddLine(ImVec2(q.x, q.y - L * 1.5f), ImVec2(q.x, q.y + L * 1.5f), IM_COL32(255, 244, 214, static_cast<int>(a * 235.f)), 1.5f);
        dl->AddLine(ImVec2(q.x - L * 0.45f, q.y - L * 0.45f), ImVec2(q.x + L * 0.45f, q.y + L * 0.45f), IM_COL32(255, 230, 170, static_cast<int>(a * 140.f)), 1.f);
        dl->AddLine(ImVec2(q.x - L * 0.45f, q.y + L * 0.45f), ImVec2(q.x + L * 0.45f, q.y - L * 0.45f), IM_COL32(255, 230, 170, static_cast<int>(a * 140.f)), 1.f);
        dl->AddCircleFilled(q, 2.5f + 3.f * a, IM_COL32(255, 250, 235, static_cast<int>(a * 255.f)), 12);
      }
    }
    // the name, letter-spaced, while it holds
    const float ta = smooth(3.95f, 4.4f, tf) * (1.f - smooth(4.5f, 4.85f, tf));
    if (ta > 0.01f) {
      ImFont* font = ImGui::GetFont();
      const float fs = ImGui::GetFontSize() * 1.7f, sp = fs * 0.36f;
      const char* name = "WEZT RECORDING TOOLS";
      float w = 0; for (const char* q = name; *q; ++q) w += font->CalcTextSizeA(fs, 1e9f, 0.f, q, q + 1).x + sp;
      w -= sp;
      float x = c.x - w * 0.5f; const float y = c.y + H0 * 0.5f * 1.08f + fs * 0.9f;
      for (const char* q = name; *q; ++q) { dl->AddText(font, fs, ImVec2(x, y), gold(ta * 0.95f, 1.05f), q, q + 1); x += font->CalcTextSizeA(fs, 1e9f, 0.f, q, q + 1).x + sp; }
    }
    if (tf > 0.4f && tf < 5.6f) {
      const char* hint = "Click or press any key to skip";
      const ImVec2 ts = ImGui::CalcTextSize(hint);
      dl->AddText(ImVec2(c.x - ts.x * 0.5f, size.y - ts.y - 18.f), IM_COL32(200, 200, 200, static_cast<int>(70.f * smooth(0.4f, 1.2f, tf))), hint);
    }
  } else {   // the main window is open underneath: a gold-white flash that fades, with a bloom out of the speck
    const float f = clamp01((tf - static_cast<float>(kIntroOpen)) / static_cast<float>(kIntroEnd - kIntroOpen));
    auto warm = [](float a) { return IM_COL32(255, 240, 205, static_cast<int>(clamp01(a) * 255.f + 0.5f)); };
    dl->AddRectFilled(ImVec2(0, 0), size, warm(0.95f * std::pow(1.f - f, 2.2f)));
    dl->AddCircleFilled(c, size.x * 0.6f * std::pow(f, 0.5f), warm(0.5f * (1.f - f)), 96);
  }
}

}  // namespace wezt::ui
