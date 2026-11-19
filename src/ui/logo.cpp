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

void ramp(float t, float* rgb) {   // deep bronze -> rich gold -> bright gold -> cream highlight
  static const float stops[5][3] = {{0.17f, 0.09f, 0.015f}, {0.50f, 0.30f, 0.05f}, {0.86f, 0.62f, 0.15f}, {1.00f, 0.84f, 0.42f}, {1.00f, 0.97f, 0.82f}};
  t = std::clamp(t, 0.f, 1.f) * 4.f;
  const int i = std::min(3, static_cast<int>(t));
  const float f = t - static_cast<float>(i);
  for (int k = 0; k < 3; ++k) rgb[k] = lerp(stops[i][k], stops[i + 1][k], f);
}

float sstep(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
float hash2(int x, int y) { unsigned n = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u; n = (n ^ (n >> 13)) * 1274126177u; return static_cast<float>((n ^ (n >> 16)) & 0xffff) / 65535.f; }

// A studio reflection for polished metal, looked up by how much the surface tilts up (+1) or down (-1): a bright softbox above,
// a dark horizon line, a warm floor below. The hard contrast between them is what reads as expensive, mirror-polished gold.
float studio(float up) {
  const float s = std::clamp(up * 2.1f, -1.f, 1.f);
  const float box = sstep(0.30f, 0.62f, s);                    // the softbox
  const float dark = 1.f - sstep(0.0f, 0.22f, std::fabs(s + 0.02f));   // the horizon band
  float v = lerp(0.42f, 0.56f, sstep(-0.15f, 0.3f, s));        // warm floor rising to the mid tone
  v = lerp(v, 0.98f, box);
  return lerp(v, 0.10f, dark * 0.85f);
}

// 20 px of room on every side of the 640x216 shape for the soft shadow to fall into.
constexpr int kPad = 20;

std::vector<float> paddedCoverage(int& W, int& H) {
  W = kLogoMaskW + 2 * kPad; H = kLogoMaskH + 2 * kPad;
  std::vector<float> c(static_cast<std::size_t>(W * H), 0.f);
  for (int y = 0; y < kLogoMaskH; ++y)
    for (int x = 0; x < kLogoMaskW; ++x) c[static_cast<std::size_t>((y + kPad) * W + x + kPad)] = static_cast<float>(kLogoMask[y * kLogoMaskW + x]) / 255.f;
  return c;
}

// Strokes made a little heavier (about two pixels each side) so the mark reads solid and substantial rather than wiry.
std::vector<float> thickened(const std::vector<float>& cov, int W, int H) {
  const std::vector<float> b = blur(cov, W, H, 2);   // radius 2, a lower threshold: strokes grow about two pixels each side
  std::vector<float> o(cov.size());
  for (std::size_t i = 0; i < cov.size(); ++i) o[i] = std::max(cov[i], sstep(0.05f, 0.34f, b[i]));
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

LogoImage makeGoldLogo() {
  int W = 0, H = 0;
  const std::vector<float> base = paddedCoverage(W, H);
  const std::vector<float> cov = thickened(base, W, H);
  const std::vector<float> soft = blur(cov, W, H, 2), wide = blur(cov, W, H, 5), broad = blur(cov, W, H, 9);
  // The soft drop shadow: the shape moved down and to the right, blurred hard.
  std::vector<float> moved(cov.size(), 0.f);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) { const int sx = x - 5, sy = y - 8; if (sx >= 0 && sy >= 0) moved[static_cast<std::size_t>(y * W + x)] = cov[static_cast<std::size_t>(sy * W + sx)]; }
  const std::vector<float> shadow = blur(moved, W, H, 4);
  auto Ht = [&](int x, int y) {   // a rounded, tube-like height for every stroke
    x = std::clamp(x, 0, W - 1); y = std::clamp(y, 0, H - 1);
    const std::size_t i = static_cast<std::size_t>(y * W + x);
    return soft[i] * 0.9f + wide[i] * 1.0f + broad[i] * 0.5f;
  };
  LogoImage im; im.w = W; im.h = H; im.rgba.resize(static_cast<std::size_t>(W * H) * 4);
  const float lx = -0.55f, ly = -0.62f, lz = 0.56f, ll = std::sqrt(lx * lx + ly * ly + lz * lz);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const std::size_t idx = static_cast<std::size_t>(y * W + x);
      const float dx = (Ht(x + 1, y) - Ht(x - 1, y)) * 3.4f, dy = (Ht(x, y + 1) - Ht(x, y - 1)) * 3.4f;
      const float nl = std::sqrt(dx * dx + dy * dy + 1.f);
      const float nx = -dx / nl, ny = -dy / nl, nz = 1.f / nl;
      const float diff = std::clamp((nx * lx + ny * ly + nz * lz) / ll, 0.f, 1.f);
      const float spec = std::pow(diff, 22.f);
      const float env = studio(-ny);                                        // ny is "down" on screen: -ny is up
      const float yn = static_cast<float>(y - kPad) / static_cast<float>(kLogoMaskH);
      const float sweep = 0.5f + 0.5f * std::cos(yn * 6.2832f * 1.0f + 0.5f);   // a broad gradient over the whole mark
      const float grain = (hash2(0, y) - 0.5f) * 0.045f + (hash2(x / 18, y / 2) - 0.5f) * 0.03f;   // faint brushed streaks
      const float c = cov[idx];
      const float edge = 4.f * c * (1.f - c);                               // 1 on the anti-aliased rim, 0 inside and outside
      float t = 0.20f + 0.50f * env + 0.20f * diff + 0.10f * sweep + 0.46f * spec + grain;
      t *= 1.f - 0.50f * edge;                                              // a fine dark outline around every stroke
      t += 0.20f * sstep(0.10f, 0.0f, Ht(x + 1, y + 1) - Ht(x - 1, y - 1)) * c * c;   // a hairline of light on the top-left lip
      float rgb[3]; ramp(t, rgb);
      const float sh = std::clamp(shadow[idx] * 0.70f, 0.f, 1.f) * (1.f - c);
      const float a = c + sh;
      unsigned char* p = &im.rgba[idx * 4];
      for (int k = 0; k < 3; ++k) {
        const float col = a > 0.f ? (std::clamp(rgb[k], 0.f, 1.f) * c) / a : 0.f;   // shadow is black, gold is gold
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
