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

// The sway after landing: a slow turn left and right, a little nod, both dying out. They end where the spin back down picks up.
float holdYaw(float s) { return 0.34f * std::sin(s * 4.2f) * std::exp(-s * 0.9f); }
float holdTilt(float s) { return 0.13f * std::sin(s * 3.1f) * std::exp(-s * 0.7f); }

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
    p.yaw = holdYaw(s);
    p.tilt = holdTilt(s);
  } else if (t < kIntroOpen) {   // spinning back down to a speck, faster and faster
    const float v = (tf - static_cast<float>(kIntroHoldEnd)) / static_cast<float>(kIntroOpen - kIntroHoldEnd);
    p.scale = 1.f - 0.975f * std::pow(v, 2.2f);
    p.yaw = 2.f * 2.f * kPi * v * v + holdYaw(0.7f) * (1.f - v);
    p.tilt = -0.15f * std::sin(kPi * v) + holdTilt(0.7f) * (1.f - v);
  } else p.alpha = 0.f;
  return p;
}

}  // namespace

IntroPose introPose(double t) { const Pose p = poseAt(t); return {p.scale, p.yaw, p.tilt, p.alpha}; }

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
  const float W0 = std::min(size.x * 0.66f, size.y * 0.60f * aspect_), H0 = W0 / aspect_;
  const float d = 1.7f * W0;
  auto col = [](int r, int g, int b, float a) { return IM_COL32(r, g, b, static_cast<int>(clamp01(a) * 255.f + 0.5f)); };

  if (t < kIntroOpen && glMode_) {
    if (tf > 0.4f && tf < 5.4f) {
      const char* hint = "Click or press any key to skip";
      const ImVec2 ts = ImGui::CalcTextSize(hint);
      dl->AddText(ImVec2(c.x - ts.x * 0.5f, size.y - ts.y - 18.f), IM_COL32(190, 190, 190, static_cast<int>(55.f * smooth(0.4f, 1.2f, tf))), hint);
    }
    return;
  }
  if (t < kIntroOpen) {
    // A black room and nothing else. The only things in it: one soft pool of warm light that grows with the logo, a faint polished floor,
    // and, when the logo lands, one thin streak of light across the frame. Restraint is the point.
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(3, 3, 5, 255));
    const Pose p = poseAt(t);
    const float lit = std::pow(p.scale, 0.8f) * p.alpha;
    for (int k = 0; k < 48; ++k) {
      const float r = (0.12f + 1.10f * static_cast<float>(k) / 47.f) * W0 * std::max(0.06f, p.scale);
      dl->AddCircleFilled(c, r, col(200, 165, 110, 0.0042f * lit), 80);
    }
    const float breath = tf >= static_cast<float>(kIntroGrowEnd) && tf < static_cast<float>(kIntroGrowEnd) + 1.4f ? std::exp(-(tf - static_cast<float>(kIntroGrowEnd)) * 2.2f) : 0.f;
    if (breath > 0.01f) {   // the landing: a single hairline of light, and the room brightens for an instant
      const float h = 1.5f + 2.5f * breath;
      const ImU32 z = col(255, 235, 200, 0.f), m = col(255, 238, 205, 0.85f * breath);
      dl->AddRectFilledMultiColor(ImVec2(0, c.y - h * 0.5f), ImVec2(c.x, c.y + h * 0.5f), z, m, m, z);
      dl->AddRectFilledMultiColor(ImVec2(c.x, c.y - h * 0.5f), ImVec2(size.x, c.y + h * 0.5f), m, z, z, m);
      for (int k = 0; k < 20; ++k) dl->AddCircleFilled(c, (0.2f + 0.9f * static_cast<float>(k) / 19.f) * W0, col(255, 225, 170, 0.006f * breath), 80);
    }

    if (p.alpha > 0.002f && p.scale > 0.f && gold_) {
      const float cy = std::cos(p.yaw), sy = std::sin(p.yaw), cx = std::cos(p.tilt), sx = std::sin(p.tilt);
      // zo is depth in the logo's own space (0 = the face, positive = toward the back); it turns with the logo, so the thickness shows
      // as side walls whenever the logo is not facing straight at the viewer.
      auto proj = [&](float lx, float ly, float zo = 0.f) {
        const float x = lx * W0 * p.scale, y = ly * H0 * p.scale, zz = zo * W0 * p.scale;
        const float x1 = x * cy + zz * sy, z1 = -x * sy + zz * cy;
        const float y2 = y * cx - z1 * sx, z2 = y * sx + z1 * cx;
        const float k = d / std::max(0.2f * d, d + z2);
        return ImVec2(c.x + x1 * k, c.y + y2 * k);
      };
      const float face = 0.45f + 0.55f * std::pow(std::fabs(cy), 0.5f);   // turned edge-on it falls into shadow
      const int a = static_cast<int>(clamp01(p.alpha) * 255.f + 0.5f), v = static_cast<int>(clamp01(face) * 255.f + 0.5f);
      const ImU32 tint = IM_COL32(v, v, v, a);
      constexpr int N = 64;

      {  // the thickness: copies of the outline stacked behind the face, lit at the front edge and falling to black at the back
        constexpr int L = 22; constexpr int NS = 12;
        const float depth = 0.055f;
        dl->PushTextureID(white_ ? white_ : gold_);
        for (int k = L; k >= 1; --k) {
          const float f = static_cast<float>(k) / L, m = std::pow(f, 0.55f);
          const float r0 = black_ ? 62.f : 176.f, g0 = black_ ? 58.f : 140.f, b0 = black_ ? 54.f : 88.f;
          const float r1 = black_ ? 5.f : 22.f, g1 = black_ ? 5.f : 17.f, b1 = black_ ? 6.f : 9.f;
          const ImU32 sideCol = IM_COL32(static_cast<int>((r0 + (r1 - r0) * m) * face), static_cast<int>((g0 + (g1 - g0) * m) * face), static_cast<int>((b0 + (b1 - b0) * m) * face), a);
          const float zo = depth * f;
          for (int s = 0; s < NS; ++s) {
            const float u0 = static_cast<float>(s) / NS, u1 = static_cast<float>(s + 1) / NS;
            dl->PrimReserve(6, 4);
            dl->PrimQuadUV(proj(u0 - 0.5f, -0.5f, zo), proj(u1 - 0.5f, -0.5f, zo), proj(u1 - 0.5f, 0.5f, zo), proj(u0 - 0.5f, 0.5f, zo),
                           ImVec2(u0, 0), ImVec2(u1, 0), ImVec2(u1, 1), ImVec2(u0, 1), sideCol);
          }
        }
        dl->PopTextureID();
      }
      {  // the polished floor: the logo mirrored beneath it, faint, gone within a short distance
        const float gap = 0.22f, top = 0.20f * clamp01(p.alpha);
        auto colA = [&](float al) { return IM_COL32(v, v, v, static_cast<int>(clamp01(al) * 255.f)); };
        dl->PushTextureID(gold_);
        for (int s = 0; s < N; ++s) {
          const float u0 = static_cast<float>(s) / N, u1 = static_cast<float>(s + 1) / N;
          dl->PrimReserve(6, 4);
          dl->PrimQuadUV(proj(u0 - 0.5f, 0.5f + gap), proj(u1 - 0.5f, 0.5f + gap), proj(u1 - 0.5f, 1.1f + gap), proj(u0 - 0.5f, 1.1f + gap),
                         ImVec2(u0, 1), ImVec2(u1, 1), ImVec2(u1, 0.45f), ImVec2(u0, 0.45f), colA(top));
          ImDrawVert* vtx = dl->_VtxWritePtr - 4;
          vtx[2].col = vtx[3].col = colA(0.f);
        }
        dl->PopTextureID();
      }
      for (int s = 0; s < N; ++s) {   // the face
        const float u0 = static_cast<float>(s) / N, u1 = static_cast<float>(s + 1) / N;
        dl->AddImageQuad(gold_, proj(u0 - 0.5f, -0.5f), proj(u1 - 0.5f, -0.5f), proj(u1 - 0.5f, 0.5f), proj(u0 - 0.5f, 0.5f), ImVec2(u0, 0), ImVec2(u1, 0), ImVec2(u1, 1), ImVec2(u0, 1), tint);
      }
      // One slow band of light travelling across the face (twice while it grows, once as it lands), shaded per vertex so it is a smooth gradient.
      static const float sweeps[3][2] = {{1.3f, 1.4f}, {2.5f, 1.3f}, {3.9f, 1.1f}};
      float sweep = -1.f;
      for (const auto& sw : sweeps) if (tf >= sw[0] && tf < sw[0] + sw[1]) sweep = -0.3f + 1.6f * smooth(0.f, 1.f, (tf - sw[0]) / sw[1]);
      if (sweep > -0.9f && white_) {
        auto band = [&](float uu) { const float dd = (uu - sweep) / 0.16f; return std::exp(-dd * dd) * (black_ ? 0.32f : 0.42f) * clamp01(p.alpha); };
        dl->PushTextureID(white_);
        for (int s = 0; s < N; ++s) {
          const float u0 = static_cast<float>(s) / N, u1 = static_cast<float>(s + 1) / N, g0 = band(u0), g1 = band(u1);
          if (g0 < 0.004f && g1 < 0.004f) continue;
          dl->PrimReserve(6, 4);
          dl->PrimQuadUV(proj(u0 - 0.5f, -0.5f), proj(u1 - 0.5f, -0.5f), proj(u1 - 0.5f, 0.5f), proj(u0 - 0.5f, 0.5f), ImVec2(u0, 0), ImVec2(u1, 0), ImVec2(u1, 1), ImVec2(u0, 1), IM_COL32(255, 244, 222, 255));
          ImDrawVert* vtx = dl->_VtxWritePtr - 4;
          vtx[0].col = vtx[3].col = IM_COL32(255, 244, 222, static_cast<int>(g0 * 255.f));
          vtx[1].col = vtx[2].col = IM_COL32(255, 244, 222, static_cast<int>(g1 * 255.f));
        }
        dl->PopTextureID();
      }
    }
    {  // the edges of the frame fall to black
      const ImU32 dk = IM_COL32(0, 0, 0, 215), cl = IM_COL32(0, 0, 0, 0);
      const float ex = size.x * 0.30f, ey = size.y * 0.32f;
      dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(ex, size.y), dk, cl, cl, dk);
      dl->AddRectFilledMultiColor(ImVec2(size.x - ex, 0), ImVec2(size.x, size.y), cl, dk, dk, cl);
      dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, ey), dk, dk, cl, cl);
      dl->AddRectFilledMultiColor(ImVec2(0, size.y - ey), ImVec2(size.x, size.y), cl, cl, dk, dk);
    }
    // The name: small, pale, widely spaced, fading in under the logo as it lands and out as it leaves.
    const float ta = smooth(4.05f, 4.6f, tf) * (1.f - smooth(4.55f, 4.85f, tf));
    if (ta > 0.01f) {
      ImFont* font = ImGui::GetFont();
      const float fs = ImGui::GetFontSize() * 1.15f, sp = fs * 0.62f;
      const char* name = "WEZT  RECORDING  TOOLS";
      float w = 0; for (const char* q = name; *q; ++q) w += font->CalcTextSizeA(fs, 1e9f, 0.f, q, q + 1).x + sp;
      w -= sp;
      float x = c.x - w * 0.5f; const float y = c.y + H0 * 0.5f + fs * 2.2f;
      for (const char* q = name; *q; ++q) { dl->AddText(font, fs, ImVec2(x, y), col(226, 208, 170, ta * 0.9f), q, q + 1); x += font->CalcTextSizeA(fs, 1e9f, 0.f, q, q + 1).x + sp; }
      const float lw = w * 0.5f * smooth(4.05f, 4.75f, tf);   // a hairline that draws out from the centre under the name
      dl->AddRectFilledMultiColor(ImVec2(c.x - lw, y + fs * 2.0f), ImVec2(c.x, y + fs * 2.0f + 1.f), col(226, 208, 170, 0.f), col(226, 208, 170, ta * 0.6f), col(226, 208, 170, ta * 0.6f), col(226, 208, 170, 0.f));
      dl->AddRectFilledMultiColor(ImVec2(c.x, y + fs * 2.0f), ImVec2(c.x + lw, y + fs * 2.0f + 1.f), col(226, 208, 170, ta * 0.6f), col(226, 208, 170, 0.f), col(226, 208, 170, 0.f), col(226, 208, 170, ta * 0.6f));
    }
    if (tf > 0.4f && tf < 5.6f) {
      const char* hint = "Click or press any key to skip";
      const ImVec2 ts = ImGui::CalcTextSize(hint);
      dl->AddText(ImVec2(c.x - ts.x * 0.5f, size.y - ts.y - 18.f), IM_COL32(190, 190, 190, static_cast<int>(55.f * smooth(0.4f, 1.2f, tf))), hint);
    }
  } else {   // the studio is open underneath: a warm white wash that clears quickly
    const float f = clamp01((tf - static_cast<float>(kIntroOpen)) / static_cast<float>(kIntroEnd - kIntroOpen));
    dl->AddRectFilled(ImVec2(0, 0), size, col(255, 240, 215, 0.9f * std::pow(1.f - f, 2.6f)));
  }
}

}  // namespace wezt::ui
