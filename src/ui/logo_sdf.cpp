#include "ui/logo_sdf.h"

#include <algorithm>
#include <cmath>

#include "ui/logo_mask.inc"

namespace wezt::ui {
namespace {

constexpr float kInf = 1e20f;

void dt1d(const float* f, int n, float* out, int* v, float* z) {
  int k = 0;
  v[0] = 0; z[0] = -kInf; z[1] = kInf;
  for (int q = 1; q < n; ++q) {
    float s;
    for (;;) {
      const int p = v[k];
      s = ((f[q] + static_cast<float>(q) * static_cast<float>(q)) - (f[p] + static_cast<float>(p) * static_cast<float>(p))) / (2.f * static_cast<float>(q - p));
      if (s <= z[k]) --k; else break;   // z[0] is -infinity, so k never goes below 0
    }
    ++k; v[k] = q; z[k] = s; z[k + 1] = kInf;
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < static_cast<float>(q)) ++k;
    const float dq = static_cast<float>(q - v[k]);
    out[q] = dq * dq + f[v[k]];
  }
}

std::vector<float> blur3(const std::vector<float>& s, int w, int h, int radius) {   // box blur x3 along both axes: smooths the pixel staircase out of the distances
  std::vector<float> a = s, b(s.size());
  auto pass = [&](const std::vector<float>& in, std::vector<float>& out, bool horiz) {
    const int len = horiz ? w : h, lines = horiz ? h : w;
    for (int l = 0; l < lines; ++l) {
      auto at = [&](int i) { return horiz ? in[static_cast<std::size_t>(l * w + i)] : in[static_cast<std::size_t>(i * w + l)]; };
      double sum = 0; int n = 0;
      for (int i = 0; i <= std::min(radius, len - 1); ++i) { sum += at(i); ++n; }
      for (int i = 0; i < len; ++i) {
        (horiz ? out[static_cast<std::size_t>(l * w + i)] : out[static_cast<std::size_t>(i * w + l)]) = static_cast<float>(sum / n);
        const int add = i + radius + 1, rem = i - radius;
        if (add < len) { sum += at(add); ++n; }
        if (rem >= 0) { sum -= at(rem); --n; }
      }
    }
  };
  for (int k = 0; k < 2; ++k) { pass(a, b, true); pass(b, a, false); }
  return a;
}

}  // namespace

void squaredDistanceTransform(std::vector<float>& f, int w, int h) {
  const int m = std::max(w, h);
  std::vector<float> col(static_cast<std::size_t>(m)), out(static_cast<std::size_t>(m)), z(static_cast<std::size_t>(m) + 1);
  std::vector<int> v(static_cast<std::size_t>(m));
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) col[static_cast<std::size_t>(y)] = f[static_cast<std::size_t>(y * w + x)];
    dt1d(col.data(), h, out.data(), v.data(), z.data());
    for (int y = 0; y < h; ++y) f[static_cast<std::size_t>(y * w + x)] = out[static_cast<std::size_t>(y)];
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) col[static_cast<std::size_t>(x)] = f[static_cast<std::size_t>(y * w + x)];
    dt1d(col.data(), w, out.data(), v.data(), z.data());
    for (int x = 0; x < w; ++x) f[static_cast<std::size_t>(y * w + x)] = out[static_cast<std::size_t>(x)];
  }
}

LogoSdf makeLogoSdf() {
  constexpr int M = 96;                                   // margin, texels
  LogoSdf s;
  s.w = kLogoMaskW + 2 * M; s.h = kLogoMaskH + 2 * M;
  const std::size_t n = static_cast<std::size_t>(s.w) * static_cast<std::size_t>(s.h);
  std::vector<float> inside(n, kInf), outside(n, kInf);   // distance to the nearest outside cell / inside cell
  for (int y = 0; y < s.h; ++y)
    for (int x = 0; x < s.w; ++x) {
      const int mx = x - M, my = y - M;
      const bool in = mx >= 0 && my >= 0 && mx < kLogoMaskW && my < kLogoMaskH && kLogoMask[my * kLogoMaskW + mx] >= 128;
      const std::size_t i = static_cast<std::size_t>(y * s.w + x);
      if (in) outside[i] = 0.f; else inside[i] = 0.f;
    }
  squaredDistanceTransform(inside, s.w, s.h);    // for inside cells: distance to the nearest outside cell
  squaredDistanceTransform(outside, s.w, s.h);   // for outside cells: distance to the nearest inside cell
  std::vector<float> d(n);
  for (std::size_t i = 0; i < n; ++i) {
    // cell centres are half a cell apart from the true boundary: -0.5 inside, +0.5 outside puts the zero level on the edge
    d[i] = inside[i] > 0.f ? -(std::sqrt(inside[i]) - 0.5f) : (std::sqrt(outside[i]) - 0.5f);
  }
  d = blur3(d, s.w, s.h, 2);
  const float unit = 2.f / static_cast<float>(kLogoMaskW);   // object units per texel
  for (float& v : d) v *= unit;
  s.d = std::move(d);
  s.halfX = unit * static_cast<float>(s.w) * 0.5f;
  s.halfY = unit * static_cast<float>(s.h) * 0.5f;
  s.logoHalfY = unit * static_cast<float>(kLogoMaskH) * 0.5f;
  return s;
}

}  // namespace wezt::ui
