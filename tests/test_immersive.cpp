// Immersive audio: layouts, VBAP, object renderer, binaural monitor, BS.1770 loudness, ADM BWF export.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include "immersive/adm.h"
#include "immersive/binaural.h"
#include "immersive/layout.h"
#include "immersive/loudness.h"
#include "immersive/renderer.h"

using namespace wezt;
using namespace wezt::immersive;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("FAIL %s:%d %s=%g vs %g\n", __FILE__, __LINE__, #a, _a, _b); ++failures; } } while (0)

static constexpr double kPi = 3.14159265358979323846;
static void dirFrom(float azDeg, float elDeg, float& x, float& y, float& z) {
  const float az = azDeg * static_cast<float>(kPi) / 180.f, el = elDeg * static_cast<float>(kPi) / 180.f;
  x = -std::sin(az) * std::cos(el); y = std::cos(az) * std::cos(el); z = std::sin(el);
}

static void testLayouts() {
  const std::map<std::string, std::size_t> sizes = {{"2.0", 2}, {"5.1", 6}, {"7.1", 8}, {"5.1.2", 8}, {"7.1.2", 10}, {"7.1.4", 12}, {"9.1.6", 16}};
  for (const auto& n : layoutNames()) {
    const auto l = makeLayout(n);
    CHECK(l.name == n && l.size() == sizes.at(n));
    CHECK((n == "2.0") ? l.lfeIndex() < 0 : l.lfeIndex() == 3);
    CHECK(l.hasHeight() == (n.size() > 3));
  }
  CHECK(makeLayout("13.37").size() == 0 && makeLayout("13.37").name.empty());
  const auto l = makeLayout("7.1.4");
  CHECK(admSpeakerLabel(l.speakers[0]) == "M+030" && admSpeakerLabel(l.speakers[1]) == "M-030" && admSpeakerLabel(l.speakers[2]) == "M+000");
  CHECK(admSpeakerLabel(l.speakers[3]) == "LFE1" && admSpeakerLabel(l.speakers[8]) == "U+045" && admSpeakerLabel(l.speakers[11]) == "U-135");
}

static void testVbap() {
  for (const auto& n : layoutNames()) {
    const auto layout = makeLayout(n);
    Vbap v(layout);
    const std::size_t N = layout.size();
    std::vector<float> g(N), prev(N);
    float worstPower = 0.f, worstJump = 0.f;
    for (float el : {0.f, 10.f, 25.f, 45.f, 70.f, 90.f}) {
      bool havePrev = false;
      for (float az = 0.f; az <= 360.f; az += 0.5f) {
        float x, y, z;
        dirFrom(az, el, x, y, z);
        v.gains(x, y, z, g.data());
        float p = 0.f;
        for (std::size_t i = 0; i < N; ++i) { p += g[i] * g[i]; CHECK(g[i] >= 0.f); if (layout.speakers[i].lfe) CHECK(g[i] == 0.f); }
        worstPower = std::max(worstPower, std::fabs(p - 1.f));
        if (havePrev && !(n == "2.0")) for (std::size_t i = 0; i < N; ++i) worstJump = std::max(worstJump, std::fabs(g[i] - prev[i]));
        prev = g; havePrev = true;
      }
    }
    CHECK(worstPower < 1e-3f);
    if (n != "2.0") { if (worstJump >= 0.25f) std::printf("layout %s jump %g\n", n.c_str(), worstJump); CHECK(worstJump < 0.25f); }   // no jumps while an object circles the listener
    // exactly at a speaker: that speaker only
    for (std::size_t i = 0; i < N; ++i) {
      const Speaker& s = layout.speakers[i];
      if (s.lfe) continue;
      float x, y, z;
      dirFrom(s.azimuth, s.elevation, x, y, z);
      v.gains(x, y, z, g.data());
      if (!(g[i] > 0.99f)) std::printf("layout %s speaker %s gain %g\n", n.c_str(), s.label.c_str(), g[i]);
      CHECK(g[i] > 0.99f);
    }
  }
  // stereo: centre is equal power, outside the pair goes to the nearest speaker
  Vbap st(makeLayout("2.0"));
  float g[2];
  st.gains(0.f, 1.f, 0.f, g);
  NEAR(g[0], 0.7071, 1e-3); NEAR(g[1], 0.7071, 1e-3);
  st.gains(-1.f, 0.f, 0.f, g);
  CHECK(g[0] == 1.f && g[1] == 0.f);   // x = -1 is hard left: the left speaker only
}

static void testRenderer() {
  const auto out = makeLayout("7.1.4"), bed = makeLayout("7.1");
  ObjectRenderer r(out, bed, 2);
  const std::size_t chans = r.inputChannels();   // 8 + 2
  CHECK(chans == 10 && r.outputLayout().size() == 12);
  RtContext ctx; ctx.playing = false;
  r.setContext(&ctx);
  r.prepare(48000, 256);
  AudioBuffer in(12, 256), outb(12, 256);
  // bed identity: each bed channel appears only in the same speaker
  for (std::size_t c = 0; c < 8; ++c) {
    in.clear();
    std::fill(in.channel(c), in.channel(c) + 256, 0.5f);
    r.process(in, outb);
    for (std::size_t s = 0; s < 12; ++s) {
      const float expect = (s == c) ? 0.5f : 0.f;
      if (std::fabs(outb.channel(s)[255] - expect) > 1e-4f) { std::printf("bed %zu -> speaker %zu = %g\n", c, s, outb.channel(s)[255]); ++failures; }
    }
  }
  // object at the left speaker's position: only L
  r.object(0).x = -0.5f; r.object(0).y = 0.8660254f; r.object(0).z = 0.f;
  in.clear(); std::fill(in.channel(8), in.channel(8) + 256, 1.f);
  r.process(in, outb); r.process(in, outb);
  NEAR(outb.channel(0)[255], 1.0, 1e-3);
  for (std::size_t s = 1; s < 12; ++s) NEAR(outb.channel(s)[255], 0.0, 1e-3);
  // object in the middle of the room spreads equally over the 11 main speakers (power preserved)
  r.object(0).x = 0.f; r.object(0).y = 0.f; r.object(0).z = 0.f;
  r.process(in, outb); r.process(in, outb);
  double pw = 0; for (std::size_t s = 0; s < 12; ++s) pw += outb.channel(s)[255] * outb.channel(s)[255];
  NEAR(pw, 1.0, 1e-3);
  NEAR(outb.channel(0)[255], 1.0 / std::sqrt(11.0), 1e-3);
  NEAR(outb.channel(3)[255], 0.0, 1e-6);   // no LFE unless sent
  // between the middle and the wall the object is partly spread: many speakers inside, two at the wall
  {
    auto count = [&](float x, float y) { int n = 0; for (float g : r.gainsFor(x, y, 0.f, 0.f)) if (g > 0.05f) ++n; return n; };
    CHECK(count(0.2f, 0.2f) >= 8);
    CHECK(count(-0.5f, 0.866f) == 1);
    CHECK(count(-0.25f, 0.95f) <= 3);
  }
  // size spreads, gain and mute apply, LFE send feeds the sub
  r.object(0).x = -0.5f; r.object(0).y = 0.866f; r.object(0).size = 1.f;
  r.process(in, outb); r.process(in, outb);
  NEAR(outb.channel(0)[255], 1.0 / std::sqrt(11.0), 1e-2);
  r.object(0).size = 0.f; r.object(0).gainDb = -6.0206f; r.object(0).lfeSend = 1.f;
  r.process(in, outb); r.process(in, outb);
  NEAR(outb.channel(0)[255], 0.5, 2e-3);
  NEAR(outb.channel(3)[255], 0.5, 2e-3);
  r.object(0).mute = true;
  r.process(in, outb); r.process(in, outb);
  CHECK(outb.peak() == 0.f);
  r.object(0).mute = false;
  // moving the object between blocks never steps: a ramp over the block
  r.object(0).gainDb = 0.f; r.object(0).lfeSend = 0.f;
  r.object(0).x = -0.5f; r.object(0).y = 0.866f; r.object(0).z = 0.f;
  r.process(in, outb);
  r.object(0).x = 0.5f;   // jump to the right speaker
  r.process(in, outb);
  float maxStep = 0.f;
  for (std::size_t i = 1; i < 256; ++i) { maxStep = std::max(maxStep, std::fabs(outb.channel(0)[i] - outb.channel(0)[i - 1])); maxStep = std::max(maxStep, std::fabs(outb.channel(1)[i] - outb.channel(1)[i - 1])); }
  CHECK(maxStep < 0.02f);
  NEAR(outb.channel(1)[255], 1.0, 2e-2);
  CHECK(r.takePeak(1) > 0.9f && r.takePeak(1) == 0.f);
  // automation: while playing, the position follows the lane
  auto& o = r.object(1);
  o.x = -0.5f; o.y = 0.866f; o.z = 0.f;
  o.laneX.add(0, -0.5f); o.laneX.add(100000, 0.5f);
  o.laneY.add(0, 0.866f); o.laneY.add(100000, 0.866f);
  r.object(0).mute = true;
  in.clear(); std::fill(in.channel(9), in.channel(9) + 256, 1.f);
  ctx.playing = true; ctx.chunkStart = 0;
  r.process(in, outb); r.process(in, outb);
  NEAR(outb.channel(0)[255], 1.0, 2e-2);       // at the start: left
  ctx.chunkStart = 100000 - 256;
  r.process(in, outb); r.process(in, outb);
  NEAR(outb.channel(1)[255], 1.0, 2e-2);       // at the end: right
  ctx.playing = false;
  // too few input channels or output channels never crash
  AudioBuffer small(2, 256), tiny(3, 256);
  r.process(small, outb);
  r.process(in, tiny);
  CHECK(true);
}

static void testBinaural() {
  const auto layout = makeLayout("5.1");
  BinauralMonitor b(layout);
  b.prepare(48000.0, 512);
  AudioBuffer in(6, 512), out(2, 512);
  auto first = [&](const float* d, float thr) { for (int i = 0; i < 512; ++i) if (std::fabs(d[i]) > thr) return i; return -1; };
  auto energy = [&](const float* d) { double e = 0; for (int i = 0; i < 512; ++i) e += d[i] * d[i]; return e; };
  in.clear(); in.channel(0)[0] = 1.f;   // impulse in L (30 degrees left)
  b.process(in, out);
  CHECK(first(out.channel(0), 0.5f) == 0);          // near ear, no delay
  const int rightFirst = first(out.channel(1), 0.01f);
  CHECK(rightFirst >= 10 && rightFirst <= 16);       // about 0.26 ms later at 48 kHz
  CHECK(energy(out.channel(0)) > 2.0 * energy(out.channel(1)));
  // mirror image
  b.prepare(48000.0, 512);
  AudioBuffer out2(2, 512);
  in.clear(); in.channel(1)[0] = 1.f;
  b.process(in, out2);
  bool mirror = true;
  for (int i = 0; i < 512; ++i) mirror = mirror && out2.channel(1)[i] == out.channel(0)[i] && out2.channel(0)[i] == out.channel(1)[i];
  CHECK(mirror);
  // centre and sub: identical in both ears
  b.prepare(48000.0, 512);
  in.clear(); in.channel(2)[0] = 1.f; in.channel(3)[5] = 0.5f;
  b.process(in, out);
  bool same = true;
  for (int i = 0; i < 512; ++i) same = same && out.channel(0)[i] == out.channel(1)[i];
  CHECK(same && out.peak() > 0.4f);
  // a side speaker is delayed more and darker than a front one
  const auto l71 = makeLayout("7.1");
  BinauralMonitor s(l71);
  s.prepare(48000.0, 512);
  AudioBuffer in8(8, 512);
  in8.clear(); in8.channel(4)[0] = 1.f;   // Lss at 90 degrees
  s.process(in8, out);
  const int sideFirst = first(out.channel(1), 0.01f);
  CHECK(sideFirst > rightFirst && sideFirst >= 28 && sideFirst <= 36);  // ~0.66 ms
  // blocks of any size give the same result as one big block
  BinauralMonitor c1(layout), c2(layout);
  c1.prepare(48000.0, 512); c2.prepare(48000.0, 512);
  AudioBuffer big(6, 512), o1(2, 512), a(6, 100), oa(2, 100);
  for (int i = 0; i < 512; ++i) big.channel(0)[i] = std::sin(0.05f * static_cast<float>(i));
  c1.process(big, o1);
  std::vector<float> cat0, cat1;
  for (int off = 0; off < 512; off += 100) {
    const int n = std::min(100, 512 - off);
    a.setFrames(static_cast<std::size_t>(n)); oa.setFrames(static_cast<std::size_t>(n));
    a.clear();
    for (int i = 0; i < n; ++i) a.channel(0)[i] = big.channel(0)[off + i];
    c2.process(a, oa);
    for (int i = 0; i < n; ++i) { cat0.push_back(oa.channel(0)[i]); cat1.push_back(oa.channel(1)[i]); }
  }
  bool eq = true;
  for (int i = 0; i < 512; ++i) eq = eq && std::fabs(cat0[static_cast<std::size_t>(i)] - o1.channel(0)[i]) < 1e-6f && std::fabs(cat1[static_cast<std::size_t>(i)] - o1.channel(1)[i]) < 1e-6f;
  CHECK(eq);
}

static std::vector<std::vector<float>> tone(std::size_t channels, double hz, double dbfs, double seconds, double rate, std::vector<bool> on = {}) {
  std::vector<std::vector<float>> d(channels, std::vector<float>(static_cast<std::size_t>(seconds * rate), 0.f));
  const double a = std::pow(10.0, dbfs / 20.0);
  for (std::size_t c = 0; c < channels; ++c) {
    if (!on.empty() && !on[c]) continue;
    for (std::size_t i = 0; i < d[c].size(); ++i) d[c][i] = static_cast<float>(a * std::sin(2 * kPi * hz * static_cast<double>(i) / rate));
  }
  return d;
}
static void feedAll(LoudnessMeter& m, const std::vector<std::vector<float>>& d, std::size_t block = 480) {
  std::vector<const float*> p(d.size());
  const std::size_t n = d[0].size();
  for (std::size_t off = 0; off < n; off += block) {
    for (std::size_t c = 0; c < d.size(); ++c) p[c] = d[c].data() + off;
    m.feed(p.data(), std::min(block, n - off));
  }
}

static void testLoudness() {
  for (double rate : {44100.0, 48000.0, 96000.0}) {
    LoudnessMeter m(rate, makeLayout("2.0"));
    feedAll(m, tone(2, 1000.0, -23.0, 10.0, rate));
    NEAR(m.integrated(), -23.0, 0.15);
    NEAR(m.momentary(), -23.0, 0.15);
    NEAR(m.shortTerm(), -23.0, 0.15);
    NEAR(m.samplePeakDb(), -23.0, 0.05);
  }
  // louder by 10 dB reads 10 LU higher; the block size does not matter
  LoudnessMeter a(48000.0, makeLayout("2.0")), b(48000.0, makeLayout("2.0"));
  feedAll(a, tone(2, 1000.0, -13.0, 5.0, 48000.0), 480);
  feedAll(b, tone(2, 1000.0, -13.0, 5.0, 48000.0), 137);
  NEAR(a.integrated(), -13.0, 0.15);
  NEAR(a.integrated(), b.integrated(), 0.01);
  // gating: loud part, then a part 20 LU lower, then silence: only the loud part counts
  LoudnessMeter g(48000.0, makeLayout("2.0"));
  feedAll(g, tone(2, 1000.0, -20.0, 10.0, 48000.0));
  feedAll(g, tone(2, 1000.0, -40.0, 10.0, 48000.0));
  feedAll(g, tone(2, 1000.0, -90.0, 10.0, 48000.0));
  NEAR(g.integrated(), -20.0, 0.2);
  CHECK(g.momentary() < -60.f);
  // surround channels count +1.5 dB, the LFE not at all, one channel is 3 dB below two
  const auto l51 = makeLayout("5.1");
  LoudnessMeter s(48000.0, l51), f(48000.0, l51), lfe(48000.0, l51);
  feedAll(s, tone(6, 1000.0, -23.0, 6.0, 48000.0, {false, false, false, false, true, false}));
  feedAll(f, tone(6, 1000.0, -23.0, 6.0, 48000.0, {true, false, false, false, false, false}));
  feedAll(lfe, tone(6, 1000.0, -3.0, 6.0, 48000.0, {false, false, false, true, false, false}));
  NEAR(f.integrated(), -23.0 - 3.0103, 0.15);
  NEAR(s.integrated() - f.integrated(), 1.49, 0.1);
  CHECK(lfe.integrated() < -150.f);
  // true peak finds inter-sample peaks: fs/4 sine at 45 degrees has samples of 0.707 but peaks at 1.0
  LoudnessMeter t(48000.0, makeLayout("2.0"));
  std::vector<std::vector<float>> q(2, std::vector<float>(4800));
  for (std::size_t i = 0; i < 4800; ++i) q[0][i] = q[1][i] = static_cast<float>(std::sin(2 * kPi * 0.25 * static_cast<double>(i) + kPi / 4));
  feedAll(t, q);
  NEAR(t.samplePeakDb(), -3.01, 0.05);
  CHECK(t.truePeakDb() > -0.5f && t.truePeakDb() < 0.3f);
  // reset and silence
  t.reset();
  CHECK(t.integrated() < -150.f && t.truePeakDb() < -150.f && t.momentary() < -150.f);
  LoudnessMeter z(48000.0, makeLayout("2.0"));
  feedAll(z, tone(2, 1000.0, -120.0, 5.0, 48000.0));
  CHECK(z.integrated() < -150.f);
}

// -------- ADM --------
static bool wellFormed(const std::string& x) {
  std::vector<std::string> st;
  for (std::size_t i = 0; i < x.size();) {
    if (x[i] != '<') { ++i; continue; }
    const auto e = x.find('>', i);
    if (e == std::string::npos) return false;
    const std::string tag = x.substr(i + 1, e - i - 1);
    i = e + 1;
    if (tag.empty()) return false;
    if (tag[0] == '?') continue;
    if (tag[0] == '/') { if (st.empty() || st.back() != tag.substr(1)) return false; st.pop_back(); continue; }
    if (tag.back() == '/') continue;
    st.push_back(tag.substr(0, tag.find_first_of(" \t\n")));
  }
  return st.empty();
}
static std::map<std::string, std::vector<std::uint8_t>> chunksOf(const std::vector<std::uint8_t>& f, std::uint32_t* riffSize = nullptr) {
  std::map<std::string, std::vector<std::uint8_t>> m;
  auto u32 = [&](std::size_t at) { return static_cast<std::uint32_t>(f[at] | f[at + 1] << 8 | f[at + 2] << 16 | static_cast<std::uint32_t>(f[at + 3]) << 24); };
  if (riffSize) *riffSize = u32(4);
  std::size_t pos = 12;
  while (pos + 8 <= f.size()) {
    const std::string tag(reinterpret_cast<const char*>(&f[pos]), 4);
    const std::size_t sz = u32(pos + 4);
    const std::size_t take = std::min(sz, f.size() - pos - 8);
    m[tag].assign(f.begin() + static_cast<std::ptrdiff_t>(pos + 8), f.begin() + static_cast<std::ptrdiff_t>(pos + 8 + take));
    pos += 8 + sz + (sz & 1);
  }
  return m;
}

static void testAdm() {
  CHECK(xmlEscape("Dialog & <FX> \"x\" 'y'") == "Dialog &amp; &lt;FX&gt; &quot;x&quot; &apos;y&apos;");
  CHECK(xmlEscape("a\x01z") == "a?z");
  CHECK(admTime(3661.5) == "01:01:01.50000" && admTime(0) == "00:00:00.00000" && admTime(-5) == "00:00:00.00000" && admTime(0.123456) == "00:00:00.12346");

  AdmProgramme p;
  p.name = "Test <Mix> & more";
  p.bed = makeLayout("5.1");
  AdmObjectTrack o1; o1.name = "Dialog & FX";
  o1.blocks = {{0.0, 1.5, -0.5f, 0.866f, 0.f, 1.f, 0.f}, {1.5, 0.0, 0.5f, 0.2f, 0.7f, 0.5f, 0.25f}};
  AdmObjectTrack o2; o2.name = "Helicopter";
  p.objects = {o1, o2};
  CHECK(p.channelCount() == 8);
  const std::string xml = buildAdmXml(p, 3.0);
  CHECK(wellFormed(xml));
  CHECK(xml.find("audioObjectName=\"Dialog &amp; FX\"") != std::string::npos);
  CHECK(xml.find("audioProgrammeName=\"Test &lt;Mix&gt; &amp; more\"") != std::string::npos);
  CHECK(xml.find("rtime=\"00:00:01.50000\" duration=\"00:00:01.50000\"") != std::string::npos);   // last block runs to the end
  CHECK(xml.find("<speakerLabel>M+030</speakerLabel>") != std::string::npos && xml.find("<speakerLabel>LFE1</speakerLabel>") != std::string::npos);
  std::size_t blocks = 0, uids = 0;
  for (auto at = xml.find("<audioBlockFormat"); at != std::string::npos; at = xml.find("<audioBlockFormat", at + 1)) ++blocks;
  for (auto at = xml.find("<audioTrackUID "); at != std::string::npos; at = xml.find("<audioTrackUID ", at + 1)) ++uids;
  CHECK(blocks == 6 + 3 && uids == 8);
  const auto chna = buildChna(p);
  CHECK(chna.size() == 4 + 40 * 8 && chna[0] == 8 && chna[2] == 8);
  CHECK(std::memcmp(&chna[4 + 40 * 7 + 2], "ATU_00000008", 12) == 0 && std::memcmp(&chna[4 + 40 * 7 + 14], "AT_00031002_01", 14) == 0 && std::memcmp(&chna[4 + 40 * 7 + 28], "AP_00031002", 11) == 0);
  CHECK(chna[4 + 40 * 7] == 8);   // track index is 1-based

  const fs::path dir = fs::temp_directory_path() / "wezt_adm";
  fs::remove_all(dir); fs::create_directories(dir);
  const std::string path = (dir / "mix.wav").string();
  AdmWriter w;
  std::string err;
  CHECK(w.open(path, p, io::WavSampleType::Int24, &err));
  std::vector<std::vector<float>> data(8, std::vector<float>(48001));  // odd frame count: padding paths
  for (std::size_t c = 0; c < 8; ++c) for (std::size_t i = 0; i < data[c].size(); ++i) data[c][i] = 0.1f * static_cast<float>(c + 1);
  std::vector<const float*> ptr(8);
  for (std::size_t c = 0; c < 8; ++c) ptr[c] = data[c].data();
  CHECK(w.write(ptr.data(), 48001) && w.finish(&err));
  std::ifstream f(path, std::ios::binary);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::uint32_t riff = 0;
  const auto ch = chunksOf(bytes, &riff);
  CHECK(riff + 8 == bytes.size());
  CHECK(ch.count("fmt ") && ch.count("data") && ch.count("chna") && ch.count("axml"));
  CHECK(ch.at("chna") == chna);
  const std::string stored(ch.at("axml").begin(), ch.at("axml").end());
  CHECK(wellFormed(stored) && stored.find("rtime=\"00:00:01.50000\" duration=\"00:00:00.50000\"") == std::string::npos);
  CHECK(stored.find("audioProgrammeID=\"APR_1001\"") != std::string::npos && stored.find("end=\"00:00:01.00002\"") != std::string::npos);
  CHECK(ch.at("data").size() == 48001 * 8 * 3);
  // a normal reader still reads the audio
  io::WavReader rd;
  CHECK(rd.open(path, &err) && rd.info().channels == 8 && rd.info().frames == 48001);
  std::vector<std::vector<float>> back(8, std::vector<float>(100));
  std::vector<float*> bp(8);
  for (std::size_t c = 0; c < 8; ++c) bp[c] = back[c].data();
  CHECK(rd.read(0, 100, bp.data()) == 100);
  NEAR(back[7][50], 0.8, 1e-5);
  // bad programmes
  AdmWriter bad;
  AdmProgramme empty;
  CHECK(!bad.open((dir / "x.wav").string(), empty, io::WavSampleType::Int24, &err) && !err.empty());
  CHECK(!bad.write(ptr.data(), 10) && !bad.finish(&err));
  AdmWriter nodir;
  CHECK(!nodir.open((dir / "no" / "such" / "x.wav").string(), p, io::WavSampleType::Int24, &err));
}

int main() {
  testLayouts();
  testVbap();
  testRenderer();
  testBinaural();
  testLoudness();
  testAdm();
  std::printf(failures ? "FAILED\n" : "immersive tests passed\n");
  return failures;
}
