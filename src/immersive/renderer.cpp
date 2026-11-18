#include "immersive/renderer.h"

#include <algorithm>
#include <cmath>

namespace wezt::immersive {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kEps = 1e-4f;

std::array<float, 3> dirOf(const Speaker& s) {
  const float az = s.azimuth * kPi / 180.f, el = s.elevation * kPi / 180.f;
  return {-std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)};
}
float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
std::array<float, 3> sub(const std::array<float, 3>& a, const std::array<float, 3>& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
}  // namespace

Vbap::Vbap(const SpeakerLayout& layout) : layout_(layout) {
  for (std::size_t i = 0; i < layout.speakers.size(); ++i) {
    if (layout.speakers[i].lfe) continue;
    main_.push_back(static_cast<int>(i));
    dir_.push_back(dirOf(layout.speakers[i]));
  }
  horizontalOnly_ = !layout.hasHeight();
  const int n = static_cast<int>(main_.size());
  if (horizontalOnly_) {
    // Neighbouring pairs around the circle; a pair is usable when its arc is under 180 degrees.
    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) order[static_cast<std::size_t>(i)] = i;
    auto ang = [&](int i) { return std::atan2(-dir_[static_cast<std::size_t>(i)][0], dir_[static_cast<std::size_t>(i)][1]); };  // azimuth, +left
    std::sort(order.begin(), order.end(), [&](int a, int b) { return ang(a) < ang(b); });
    for (int k = 0; k < n; ++k) {
      const int a = order[static_cast<std::size_t>(k)], b = order[static_cast<std::size_t>((k + 1) % n)];
      if (a == b) continue;
      float arc = ang(b) - ang(a);
      if (arc <= 0.f) arc += 2.f * kPi;
      if (arc >= kPi - 1e-3f) continue;
      const auto& A = dir_[static_cast<std::size_t>(a)];
      const auto& B = dir_[static_cast<std::size_t>(b)];
      const float det = A[0] * B[1] - B[0] * A[1];
      if (std::fabs(det) < 1e-6f) continue;
      pairs_.push_back({a, b});
      pairInv_.insert(pairInv_.end(), {B[1] / det, -B[0] / det, -A[1] / det, A[0] / det});
    }
    return;
  }
  dir_.push_back({0.f, 0.f, -1.f});  // virtual speaker below the listener closes the hull
  const int m = static_cast<int>(dir_.size());
  auto makeTri = [&](int i, int j, int k, Tri& t) {
    const auto& A = dir_[static_cast<std::size_t>(i)];
    const auto& B = dir_[static_cast<std::size_t>(j)];
    const auto& C = dir_[static_cast<std::size_t>(k)];
    const auto bc = cross(B, C), ca = cross(C, A), ab = cross(A, B);
    const float det = dot(A, bc);
    if (std::fabs(det) < 1e-6f) return false;
    t = Tri{i, j, k, {}};
    for (int c = 0; c < 3; ++c) { t.inv[c] = bc[static_cast<std::size_t>(c)] / det; t.inv[3 + c] = ca[static_cast<std::size_t>(c)] / det; t.inv[6 + c] = ab[static_cast<std::size_t>(c)] / det; }
    return true;
  };
  // Hull faces: every triple of directions with all other directions on one side of its plane.
  struct Face { int a, b, c; std::array<float, 3> n; float d; };
  std::vector<Face> faces;
  for (int i = 0; i < m; ++i)
    for (int j = i + 1; j < m; ++j)
      for (int k = j + 1; k < m; ++k) {
        const auto& P = dir_[static_cast<std::size_t>(i)];
        auto nrm = cross(sub(dir_[static_cast<std::size_t>(j)], P), sub(dir_[static_cast<std::size_t>(k)], P));
        const float len = std::sqrt(dot(nrm, nrm));
        if (len < 1e-5f) continue;
        for (auto& v : nrm) v /= len;
        bool pos = false, neg = false;
        for (int q = 0; q < m; ++q) {
          if (q == i || q == j || q == k) continue;
          const float d = dot(nrm, sub(dir_[static_cast<std::size_t>(q)], P));
          if (d > 1e-3f) pos = true; else if (d < -1e-3f) neg = true;
        }
        if (pos && neg) continue;
        float off = dot(nrm, P);
        if (off < 0.f) { for (auto& v : nrm) v = -v; off = -off; }
        faces.push_back({i, j, k, nrm, off});
      }
  // Faces in one plane (e.g. a ring of top speakers) overlap: replace each such group by a fan over its polygon.
  std::vector<bool> used(faces.size(), false);
  for (std::size_t f = 0; f < faces.size(); ++f) {
    if (used[f]) continue;
    std::vector<std::size_t> group{f};
    used[f] = true;
    for (std::size_t g = f + 1; g < faces.size(); ++g)
      if (!used[g] && std::fabs(dot(faces[f].n, faces[g].n) - 1.f) < 1e-3f && std::fabs(faces[f].d - faces[g].d) < 1e-3f) { used[g] = true; group.push_back(g); }
    std::vector<int> pts;
    for (std::size_t g : group) for (int q : {faces[g].a, faces[g].b, faces[g].c}) if (std::find(pts.begin(), pts.end(), q) == pts.end()) pts.push_back(q);
    Tri t;
    if (pts.size() == 3) { if (makeTri(pts[0], pts[1], pts[2], t)) tris_.push_back(t); continue; }
    // order the polygon around its centre, in the plane
    std::array<float, 3> c{};
    for (int q : pts) for (std::size_t d = 0; d < 3; ++d) c[d] += dir_[static_cast<std::size_t>(q)][d] / static_cast<float>(pts.size());
    const auto& n = faces[f].n;
    std::array<float, 3> u = sub(dir_[static_cast<std::size_t>(pts[0])], c);
    const float ul = std::sqrt(dot(u, u));
    if (ul < 1e-6f) continue;
    for (auto& v : u) v /= ul;
    const auto w = cross(n, u);
    std::sort(pts.begin(), pts.end(), [&](int a, int b) {
      const auto da = sub(dir_[static_cast<std::size_t>(a)], c), db = sub(dir_[static_cast<std::size_t>(b)], c);
      return std::atan2(dot(da, w), dot(da, u)) < std::atan2(dot(db, w), dot(db, u));
    });
    for (std::size_t q = 1; q + 1 < pts.size(); ++q)
      if (makeTri(pts[0], pts[q], pts[q + 1], t)) tris_.push_back(t);
  }
}

bool Vbap::solve(const Tri& t, const float p[3], float g[3]) const {
  for (int r = 0; r < 3; ++r) g[r] = t.inv[r * 3] * p[0] + t.inv[r * 3 + 1] * p[1] + t.inv[r * 3 + 2] * p[2];
  return g[0] >= -kEps && g[1] >= -kEps && g[2] >= -kEps;
}

void Vbap::gains(float x, float y, float z, float* out) const {
  for (std::size_t i = 0; i < layout_.size(); ++i) out[i] = 0.f;
  const std::size_t n = main_.size();
  if (n == 0) return;
  z = std::max(z, 0.f);
  const float len = std::sqrt(x * x + y * y + z * z);
  if (len < 1e-6f) { for (int idx : main_) out[idx] = 1.f / std::sqrt(static_cast<float>(n)); return; }
  const float p[3] = {x / len, y / len, z / len};
  auto nearest = [&] {
    std::size_t best = 0;
    float bd = -2.f;
    for (std::size_t i = 0; i < n; ++i) { const float d = p[0] * dir_[i][0] + p[1] * dir_[i][1] + p[2] * dir_[i][2]; if (d > bd) { bd = d; best = i; } }
    out[main_[best]] = 1.f;
  };
  if (horizontalOnly_) {
    // Project onto the horizontal plane (a source above the ring goes to its azimuth).
    const float h = std::sqrt(p[0] * p[0] + p[1] * p[1]);
    if (h < 1e-6f) { for (int idx : main_) out[idx] = 1.f / std::sqrt(static_cast<float>(n)); return; }
    const float q[2] = {p[0] / h, p[1] / h};
    for (std::size_t k = 0; k < pairs_.size(); ++k) {
      const float* I = &pairInv_[k * 4];
      const float g0 = I[0] * q[0] + I[1] * q[1], g1 = I[2] * q[0] + I[3] * q[1];
      if (g0 >= -kEps && g1 >= -kEps) {
        const float a = std::max(g0, 0.f), b = std::max(g1, 0.f), nrm = std::sqrt(a * a + b * b);
        if (nrm < 1e-9f) continue;
        out[main_[static_cast<std::size_t>(pairs_[k].first)]] = a / nrm;
        out[main_[static_cast<std::size_t>(pairs_[k].second)]] = b / nrm;
        return;
      }
    }
    nearest();
    return;
  }
  int best = -1;
  float bestMin = -1e9f, bg[3] = {};
  for (std::size_t k = 0; k < tris_.size(); ++k) {
    float g[3];
    for (int r = 0; r < 3; ++r) g[r] = tris_[k].inv[r * 3] * p[0] + tris_[k].inv[r * 3 + 1] * p[1] + tris_[k].inv[r * 3 + 2] * p[2];
    const float mn = std::min({g[0], g[1], g[2]});
    if (mn > bestMin) { bestMin = mn; best = static_cast<int>(k); bg[0] = g[0]; bg[1] = g[1]; bg[2] = g[2]; }
  }
  if (best < 0 || bestMin < -kEps) { nearest(); return; }
  const int ids[3] = {tris_[static_cast<std::size_t>(best)].a, tris_[static_cast<std::size_t>(best)].b, tris_[static_cast<std::size_t>(best)].c};
  float g[3], sum = 0.f;
  for (int r = 0; r < 3; ++r) { g[r] = ids[r] < static_cast<int>(n) ? std::max(bg[r], 0.f) : 0.f; sum += g[r] * g[r]; }
  if (sum < 1e-12f) { nearest(); return; }
  const float nrm = std::sqrt(sum);
  for (int r = 0; r < 3; ++r) if (ids[r] < static_cast<int>(n)) out[main_[static_cast<std::size_t>(ids[r])]] = g[r] / nrm;
}

// ---------------------------------------------------------------------------------------------
ObjectRenderer::ObjectRenderer(const SpeakerLayout& output, const SpeakerLayout& bed, std::size_t objects)
    : out_(output), bed_(bed), vbap_(output) {
  for (std::size_t i = 0; i < objects; ++i) objects_.push_back(std::make_unique<ObjectState>());
  cur_.assign(bed_.size() + objects, std::array<float, kMaxSpeakers>{});
  bedGains_.assign(bed_.size(), std::array<float, kMaxSpeakers>{});
  for (std::size_t j = 0; j < bed_.size(); ++j) {
    if (bed_.speakers[j].lfe) { const int l = out_.lfeIndex(); if (l >= 0) bedGains_[j][static_cast<std::size_t>(l)] = 1.f; continue; }
    const auto d = dirOf(bed_.speakers[j]);
    float g[kMaxSpeakers] = {};
    vbap_.gains(d[0], d[1], std::max(d[2], 0.f), g);
    for (std::size_t s = 0; s < out_.size() && s < kMaxSpeakers; ++s) bedGains_[j][s] = g[s];
  }
}

std::vector<float> ObjectRenderer::gainsFor(float x, float y, float z, float size) const {
  std::array<float, kMaxSpeakers> g{};
  float v[kMaxSpeakers] = {};
  x = std::clamp(x, -1.f, 1.f); y = std::clamp(y, -1.f, 1.f); z = std::clamp(z, 0.f, 1.f);
  vbap_.gains(x, y, z, v);
  const float rho = std::max({std::fabs(x), std::fabs(y), z});
  const float s = std::clamp(std::max(size, 1.f - std::min(1.f, rho * 1.25f)), 0.f, 1.f);
  const float n = static_cast<float>(std::max<std::size_t>(1, out_.mainCount()));
  for (std::size_t i = 0; i < out_.size() && i < kMaxSpeakers; ++i)
    g[i] = out_.speakers[i].lfe ? 0.f : std::sqrt((1.f - s) * v[i] * v[i] + s / n);
  return std::vector<float>(g.begin(), g.begin() + static_cast<std::ptrdiff_t>(std::min(out_.size(), kMaxSpeakers)));
}

void ObjectRenderer::targetGains(std::size_t o, std::int64_t frame, float* g) const {
  const ObjectState& st = *objects_[o];
  float x = st.x.load(std::memory_order_relaxed), y = st.y.load(std::memory_order_relaxed), z = st.z.load(std::memory_order_relaxed);
  if (ctx_ && ctx_->playing && automate_.load(std::memory_order_relaxed)) {
    if (!st.laneX.empty()) x = st.laneX.valueAt(frame, x);
    if (!st.laneY.empty()) y = st.laneY.valueAt(frame, y);
    if (!st.laneZ.empty()) z = st.laneZ.valueAt(frame, z);
  }
  const auto v = gainsFor(x, y, z, st.size.load(std::memory_order_relaxed));
  const float lin = st.mute.load(std::memory_order_relaxed) ? 0.f : session::dbToLin(st.gainDb.load(std::memory_order_relaxed));
  for (std::size_t i = 0; i < kMaxSpeakers; ++i) g[i] = i < v.size() ? v[i] * lin : 0.f;
  const int l = out_.lfeIndex();
  if (l >= 0) g[l] = st.lfeSend.load(std::memory_order_relaxed) * lin;
}

void ObjectRenderer::prepare(double, std::size_t) { first_ = true; }

void ObjectRenderer::process(const AudioBuffer& in, AudioBuffer& out) {
  const std::size_t n = out.frames;
  out.clearValid();
  const std::size_t nOut = std::min(out_.size(), out.channels);
  const std::int64_t endFrame = ctx_ ? ctx_->chunkStart + static_cast<std::int64_t>(n) : 0;
  const float bedLin = session::dbToLin(bedGainDb_.load(std::memory_order_relaxed));
  for (std::size_t c = 0; c < cur_.size(); ++c) {
    std::array<float, kMaxSpeakers> tg{};
    if (c < bed_.size()) { for (std::size_t s = 0; s < kMaxSpeakers; ++s) tg[s] = bedGains_[c][s] * bedLin; }
    else targetGains(c - bed_.size(), endFrame, tg.data());
    if (first_) cur_[c] = tg;
    if (c < in.channels) {
      const float* x = in.channel(c);
      for (std::size_t s = 0; s < nOut; ++s) {
        const float g0 = cur_[c][s], g1 = tg[s];
        if (g0 == 0.f && g1 == 0.f) continue;
        float* y = out.channel(s);
        if (g0 == g1) for (std::size_t i = 0; i < n; ++i) y[i] += x[i] * g1;
        else { const float st = (g1 - g0) / static_cast<float>(n ? n : 1); for (std::size_t i = 0; i < n; ++i) y[i] += x[i] * (g0 + st * static_cast<float>(i + 1)); }
      }
    }
    cur_[c] = tg;
  }
  first_ = false;
  for (std::size_t s = 0; s < nOut && s < kMaxSpeakers; ++s) {
    float p = 0.f;
    const float* y = out.channel(s);
    for (std::size_t i = 0; i < n; ++i) p = std::max(p, std::fabs(y[i]));
    float cur = peaks_[s].load(std::memory_order_relaxed);
    while (p > cur && !peaks_[s].compare_exchange_weak(cur, p, std::memory_order_relaxed)) {}
  }
}

}  // namespace wezt::immersive
