#include "ui/logo.h"

#include <algorithm>
#include <cmath>

#include "ui/logo_mask.inc"

namespace wezt::ui {
namespace {

std::vector<float> blur(const std::vector<float>& src, int w, int h, int radius) {   // three box passes ~ a gaussian; sliding sums, so cost does not grow with the radius
  std::vector<float> a = src, b(src.size());
  auto pass = [&](const std::vector<float>& in, std::vector<float>& out, bool horiz) {
    const int len = horiz ? w : h, lines = horiz ? h : w;
    for (int l = 0; l < lines; ++l) {
      auto at = [&](int i) { return horiz ? in[static_cast<std::size_t>(l * w + i)] : in[static_cast<std::size_t>(i * w + l)]; };
      float sum = 0; int n = 0;
      for (int i = 0; i <= std::min(radius, len - 1); ++i) { sum += at(i); ++n; }
      for (int i = 0; i < len; ++i) {
        (horiz ? out[static_cast<std::size_t>(l * w + i)] : out[static_cast<std::size_t>(i * w + l)]) = sum / static_cast<float>(n);
        const int add = i + radius + 1, rem = i - radius;
        if (add < len) { sum += at(add); ++n; }
        if (rem >= 0) { sum -= at(rem); --n; }
      }
    }
  };
  for (int k = 0; k < 3; ++k) { pass(a, b, true); pass(b, a, false); }
  return a;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void ramp(float t, float* rgb) {   // deep umber -> champagne gold -> pale highlight (less saturated than "yellow" gold: it reads as the real metal)
  static const float stops[5][3] = {{0.07f, 0.045f, 0.02f}, {0.36f, 0.26f, 0.12f}, {0.72f, 0.56f, 0.30f}, {0.93f, 0.80f, 0.55f}, {1.00f, 0.95f, 0.84f}};
  t = std::clamp(t, 0.f, 1.f) * 4.f;
  const int i = std::min(3, static_cast<int>(t));
  const float f = t - static_cast<float>(i);
  for (int k = 0; k < 3; ++k) rgb[k] = lerp(stops[i][k], stops[i + 1][k], f);
}

float sstep(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
float hash2(int x, int y) { unsigned n = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u; n = (n ^ (n >> 13)) * 1274126177u; return static_cast<float>((n ^ (n >> 16)) & 0xffff) / 65535.f; }

// 32 px of room on every side of the mask for the soft shadow to fall into.
constexpr int kPad = 32;

std::vector<float> paddedCoverage(int& W, int& H) {
  W = kLogoMaskW + 2 * kPad; H = kLogoMaskH + 2 * kPad;
  std::vector<float> c(static_cast<std::size_t>(W * H), 0.f);
  for (int y = 0; y < kLogoMaskH; ++y)
    for (int x = 0; x < kLogoMaskW; ++x) c[static_cast<std::size_t>((y + kPad) * W + x + kPad)] = static_cast<float>(kLogoMask[y * kLogoMaskW + x]) / 255.f;
  return c;
}

// Strokes made a little heavier (about three pixels each side) so the mark reads solid and substantial rather than wiry.
std::vector<float> thickened(const std::vector<float>& cov, int W, int H) {
  const std::vector<float> b = blur(cov, W, H, 3);   // strokes grow about three pixels each side (the mask is 1024 px wide)
  std::vector<float> o(cov.size());
  for (std::size_t i = 0; i < cov.size(); ++i) o[i] = std::max(cov[i], sstep(0.05f, 0.36f, b[i]));
  return o;
}

}  // namespace

LogoImage makeWhiteLogo() {
  int W = 0, H = 0;
  const std::vector<float> base = paddedCoverage(W, H);
  const std::vector<float> cov = thickened(base, W, H);
  LogoImage im; im.w = W; im.h = H; im.rgba.resize(static_cast<std::size_t>(W * H) * 4);
  for (std::size_t i = 0; i < cov.size(); ++i) {
    im.rgba[i * 4 + 0] = im.rgba[i * 4 + 1] = im.rgba[i * 4 + 2] = 255;
    im.rgba[i * 4 + 3] = static_cast<unsigned char>(std::clamp(cov[i], 0.f, 1.f) * 255.f + 0.5f);
  }
  return im;
}

LogoImage makeLogo(LogoStyle style) {
  int W = 0, H = 0;
  const std::vector<float> base = paddedCoverage(W, H);
  const std::vector<float> cov = thickened(base, W, H);
  // Machined metal, not a puffy emboss: the face of every stroke is flat and clean; only a narrow chamfer (a few pixels) along each edge
  // catches the light. Edges facing the light are bright, edges facing away are dark, and that crisp contrast is what reads as
  // precision-made and expensive.
  const std::vector<float> soft = blur(cov, W, H, 2), face = blur(cov, W, H, 10);
  std::vector<float> moved(cov.size(), 0.f);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) { const int sx = x - 6, sy = y - 10; if (sx >= 0 && sy >= 0) moved[static_cast<std::size_t>(y * W + x)] = cov[static_cast<std::size_t>(sy * W + sx)]; }
  const std::vector<float> shadow = blur(moved, W, H, 9);   // a soft, wide, faint shadow: depth without a muddy halo
  auto Ht = [&](int x, int y) { x = std::clamp(x, 0, W - 1); y = std::clamp(y, 0, H - 1); return soft[static_cast<std::size_t>(y * W + x)]; };
  LogoImage im; im.w = W; im.h = H; im.rgba.resize(static_cast<std::size_t>(W * H) * 4);
  const float lx = -0.60f, ly = -0.68f, lz = 0.42f, ll = std::sqrt(lx * lx + ly * ly + lz * lz);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const std::size_t idx = static_cast<std::size_t>(y * W + x);
      const float c = cov[idx];
      const float dx = (Ht(x + 1, y) - Ht(x - 1, y)) * 9.f, dy = (Ht(x, y + 1) - Ht(x, y - 1)) * 9.f;
      const float slope = std::clamp(std::sqrt(dx * dx + dy * dy) * 0.55f, 0.f, 1.f);       // 0 on the flat face, 1 on the chamfer
      const float nl = std::sqrt(dx * dx + dy * dy + 1.f);
      const float nx = -dx / nl, ny = -dy / nl, nz = 1.f / nl;
      const float lit = (nx * lx + ny * ly + nz * lz) / ll;                                  // -1..1: toward / away from the light
      const float edgeLight = std::clamp(lit, 0.f, 1.f), edgeDark = std::clamp(-lit, 0.f, 1.f);
      // a long, soft diagonal sheen across the whole mark, like a strip light being reflected in a flat polished face
      const float dn = (static_cast<float>(x - kPad) / static_cast<float>(kLogoMaskW) * 0.62f + static_cast<float>(y - kPad) / static_cast<float>(kLogoMaskH) * 0.38f);
      const float sheen = 0.5f + 0.5f * std::cos((dn - 0.30f) * 6.2832f * 1.15f);
      const float brushed = (hash2(0, y) - 0.5f) * 0.03f + (hash2(x / 40, y / 2) - 0.5f) * 0.015f;
      float t = 0.40f + 0.20f * sheen + brushed;                        // the flat face
      t = lerp(t, 0.52f + 0.48f * std::pow(edgeLight, 0.7f), slope);    // chamfer facing the light: bright
      t = lerp(t, 0.12f, slope * edgeDark * 0.85f);                     // chamfer facing away: dark
      float rgb[3]; ramp(t, rgb);
      if (style == LogoStyle::Black) {
        // Satin black face with the same soft sheen (barely there), and the chamfer picks up gold: a fine gold inlay around every stroke.
        const float tb = 0.035f + 0.075f * sheen + brushed * 0.5f;
        float body[3] = {0.012f + tb * 0.9f, 0.012f + tb * 0.9f, 0.016f + tb};
        for (int k = 0; k < 3; ++k) rgb[k] = lerp(body[k], rgb[k], std::clamp(slope * 1.15f, 0.f, 1.f));
        (void)face;
      }
      const float sh = std::clamp(shadow[idx] * 0.55f, 0.f, 1.f) * (1.f - c);
      const float a = c + sh;
      unsigned char* p = &im.rgba[idx * 4];
      for (int k = 0; k < 3; ++k) {
        const float col = a > 0.f ? (std::clamp(rgb[k], 0.f, 1.f) * c) / a : 0.f;   // shadow is black, metal is metal
        p[k] = static_cast<unsigned char>(std::clamp(col, 0.f, 1.f) * 255.f + 0.5f);
      }
      p[3] = static_cast<unsigned char>(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f);
    }
  return im;
}

std::vector<LogoImage> mipChain(const LogoImage& base) {
  std::vector<LogoImage> m{base};
  while (m.back().w > 1 || m.back().h > 1) {
    const LogoImage& s = m.back();
    LogoImage d; d.w = std::max(1, s.w / 2); d.h = std::max(1, s.h / 2);
    d.rgba.resize(static_cast<std::size_t>(d.w * d.h) * 4);
    for (int y = 0; y < d.h; ++y)
      for (int x = 0; x < d.w; ++x) {
        float acc[4] = {0, 0, 0, 0};
        for (int j = 0; j < 2; ++j)
          for (int i = 0; i < 2; ++i) {
            const int sx = std::min(s.w - 1, 2 * x + i), sy = std::min(s.h - 1, 2 * y + j);
            const unsigned char* p = &s.rgba[static_cast<std::size_t>(sy * s.w + sx) * 4];
            const float a = static_cast<float>(p[3]);
            for (int k = 0; k < 3; ++k) acc[k] += static_cast<float>(p[k]) * a;   // weight colour by coverage so empty pixels do not darken edges
            acc[3] += a;
          }
        unsigned char* o = &d.rgba[static_cast<std::size_t>(y * d.w + x) * 4];
        for (int k = 0; k < 3; ++k) o[k] = acc[3] > 0.f ? static_cast<unsigned char>(std::clamp(acc[k] / acc[3], 0.f, 255.f) + 0.5f) : 0;
        o[3] = static_cast<unsigned char>(std::clamp(acc[3] / 4.f, 0.f, 255.f) + 0.5f);
      }
    m.push_back(std::move(d));
  }
  return m;
}

}  // namespace wezt::ui
