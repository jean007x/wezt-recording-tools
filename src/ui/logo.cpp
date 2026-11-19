#include "ui/logo.h"

#include <algorithm>
#include <cmath>

#include "ui/logo_mask.inc"

namespace wezt::ui {
namespace {

std::vector<float> blur(const std::vector<float>& src, int w, int h, int radius) {   // three box passes ~ a gaussian
  std::vector<float> a = src, b(src.size());
  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < h; ++y)        // horizontal
      for (int x = 0; x < w; ++x) {
        float s = 0; int n = 0;
        for (int k = -radius; k <= radius; ++k) { const int xx = x + k; if (xx >= 0 && xx < w) { s += a[static_cast<std::size_t>(y * w + xx)]; ++n; } }
        b[static_cast<std::size_t>(y * w + x)] = s / static_cast<float>(n);
      }
    for (int y = 0; y < h; ++y)        // vertical
      for (int x = 0; x < w; ++x) {
        float s = 0; int n = 0;
        for (int k = -radius; k <= radius; ++k) { const int yy = y + k; if (yy >= 0 && yy < h) { s += b[static_cast<std::size_t>(yy * w + x)]; ++n; } }
        a[static_cast<std::size_t>(y * w + x)] = s / static_cast<float>(n);
      }
  }
  return a;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void ramp(float t, float* rgb) {   // dark bronze -> gold -> bright gold -> pale highlight
  static const float stops[5][3] = {{0.28f, 0.16f, 0.03f}, {0.62f, 0.40f, 0.08f}, {0.93f, 0.72f, 0.22f}, {1.00f, 0.88f, 0.52f}, {1.00f, 0.98f, 0.86f}};
  t = std::clamp(t, 0.f, 1.f) * 4.f;
  const int i = std::min(3, static_cast<int>(t));
  const float f = t - static_cast<float>(i);
  for (int k = 0; k < 3; ++k) rgb[k] = lerp(stops[i][k], stops[i + 1][k], f);
}

}  // namespace

LogoImage makeWhiteLogo() {
  LogoImage im; im.w = kLogoMaskW; im.h = kLogoMaskH;
  im.rgba.resize(static_cast<std::size_t>(im.w * im.h) * 4);
  for (int i = 0; i < im.w * im.h; ++i) {
    im.rgba[static_cast<std::size_t>(i) * 4 + 0] = im.rgba[static_cast<std::size_t>(i) * 4 + 1] = im.rgba[static_cast<std::size_t>(i) * 4 + 2] = 255;
    im.rgba[static_cast<std::size_t>(i) * 4 + 3] = kLogoMask[i];
  }
  return im;
}

LogoImage makeGoldLogo() {
  const int w = kLogoMaskW, h = kLogoMaskH;
  std::vector<float> cov(static_cast<std::size_t>(w * h));
  for (std::size_t i = 0; i < cov.size(); ++i) cov[i] = static_cast<float>(kLogoMask[i]) / 255.f;
  const std::vector<float> soft = blur(cov, w, h, 2), wide = blur(cov, w, h, 5);
  auto H = [&](int x, int y) { x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1); const std::size_t i = static_cast<std::size_t>(y * w + x); return soft[i] * 1.0f + wide[i] * 0.8f; };
  LogoImage im; im.w = w; im.h = h; im.rgba.resize(static_cast<std::size_t>(w * h) * 4);
  const float lx = -0.55f, ly = -0.60f, lz = 0.58f, ll = std::sqrt(lx * lx + ly * ly + lz * lz);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float dx = (H(x + 1, y) - H(x - 1, y)) * 2.6f, dy = (H(x, y + 1) - H(x, y - 1)) * 2.6f;
      const float nl = std::sqrt(dx * dx + dy * dy + 1.f);
      const float nx = -dx / nl, ny = -dy / nl, nz = 1.f / nl;
      const float diff = std::clamp((nx * lx + ny * ly + nz * lz) / ll, 0.f, 1.f);
      const float spec = std::pow(diff, 14.f);
      const float yn = static_cast<float>(y) / static_cast<float>(h);
      const float band = 0.5f + 0.5f * std::cos(yn * 6.2832f * 1.5f + 0.6f);   // soft horizontal banding like polished metal
      const float c = cov[static_cast<std::size_t>(y * w + x)];
      const float inner = std::clamp(wide[static_cast<std::size_t>(y * w + x)], 0.f, 1.f);   // darker toward the thin rim of strokes
      float t = 0.16f + 0.52f * diff + 0.20f * band + 0.34f * spec + 0.10f * inner;
      float rgb[3]; ramp(t, rgb);
      unsigned char* p = &im.rgba[static_cast<std::size_t>(y * w + x) * 4];
      for (int k = 0; k < 3; ++k) p[k] = static_cast<unsigned char>(std::clamp(rgb[k], 0.f, 1.f) * 255.f + 0.5f);
      p[3] = static_cast<unsigned char>(std::clamp(c, 0.f, 1.f) * 255.f + 0.5f);
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
