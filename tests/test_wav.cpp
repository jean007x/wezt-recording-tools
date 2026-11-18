#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "io/wav.h"

using namespace wezt;
using namespace wezt::io;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::vector<std::vector<float>> makeSignal(unsigned ch, std::size_t n) {
  std::vector<std::vector<float>> s(ch, std::vector<float>(n));
  for (unsigned c = 0; c < ch; ++c)
    for (std::size_t i = 0; i < n; ++i) s[c][i] = 0.9f * std::sin(0.01f * static_cast<float>(i) * static_cast<float>(c + 1));
  return s;
}
static std::vector<const float*> ptrs(const std::vector<std::vector<float>>& v) {
  std::vector<const float*> p;
  for (auto& x : v) p.push_back(x.data());
  return p;
}
static std::uint32_t le32(const std::string& b, std::size_t o) {
  return static_cast<std::uint8_t>(b[o]) | (static_cast<std::uint8_t>(b[o + 1]) << 8) | (static_cast<std::uint8_t>(b[o + 2]) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o + 3])) << 24);
}
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }

static void roundTrip(WavSampleType t, unsigned ch, double tol, const fs::path& dir) {
  const std::size_t n = 5000;
  auto sig = makeSignal(ch, n);
  const auto path = (dir / "rt.wav").string();
  WavWriter w;
  std::string err;
  CHECK(w.open(path, ch, 48000, t, &err));
  auto p = ptrs(sig);
  CHECK(w.write(p.data(), 1234));                       // uneven chunks on purpose
  std::vector<const float*> p2;
  for (auto& x : sig) p2.push_back(x.data() + 1234);
  CHECK(w.write(p2.data(), n - 1234));
  CHECK(w.finalize(&err));
  CHECK(w.framesWritten() == n);

  WavReader r;
  CHECK(r.open(path, &err));
  CHECK(r.info().channels == ch && r.info().sampleRate == 48000 && r.info().frames == n);
  std::vector<std::vector<float>> back(ch, std::vector<float>(n));
  std::vector<float*> bp;
  for (auto& x : back) bp.push_back(x.data());
  CHECK(r.read(0, n, bp.data()) == n);
  double worst = 0;
  for (unsigned c = 0; c < ch; ++c)
    for (std::size_t i = 0; i < n; ++i) worst = std::fmax(worst, std::fabs(back[c][i] - sig[c][i]));
  CHECK(worst <= tol);
  // random access
  std::vector<float> one(10);
  float* op[1] = {one.data()};
  WavReader r1;
  CHECK(r1.open(path, &err));
  if (ch == 1) { CHECK(r1.read(4000, 10, op) == 10); for (int i = 0; i < 10; ++i) CHECK(std::fabs(one[i] - sig[0][4000 + i]) <= tol); }
  // past the end
  std::vector<float> junk(100);
  std::vector<float*> jp(ch, junk.data());
  CHECK(r.read(n - 5, 100, jp.data()) == 5);
  CHECK(r.read(n, 10, jp.data()) == 0);
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "wezt_wav_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::string err;

  roundTrip(WavSampleType::Float32, 2, 0.0, dir);          // float is exact
  roundTrip(WavSampleType::Int24, 2, 1.0 / 8388608.0, dir);
  roundTrip(WavSampleType::Int16, 1, 1.0 / 32768.0, dir);
  roundTrip(WavSampleType::Float32, 6, 0.0, dir);          // >2 channels uses WAVE_FORMAT_EXTENSIBLE
  roundTrip(WavSampleType::Int24, 3, 1.0 / 8388608.0, dir);

  // --- BWF fields, at the offsets the EBU spec says ---
  {
    const auto path = (dir / "bwf.wav").string();
    AudioBuffer b(2, 100);
    CHECK(writeWavFloat(path, b, 44100, "take 7", 0x0000000123456789ULL));
    WavReader r;
    CHECK(r.open(path, &err));
    CHECK(r.info().hasBext && r.info().timeReference == 0x0000000123456789ULL && r.info().description == "take 7");
    const std::string raw = slurp(path);
    const auto at = raw.find("bext");
    CHECK(at != std::string::npos && le32(raw, at + 4) == 602);
    const std::size_t body = at + 8;
    CHECK(le32(raw, body + 256 + 32 + 32 + 10 + 8) == 0x23456789u);      // TimeReferenceLow
    CHECK(le32(raw, body + 256 + 32 + 32 + 10 + 8 + 4) == 0x1u);          // TimeReferenceHigh
    CHECK(static_cast<std::uint8_t>(raw[body + 256 + 32 + 32 + 10 + 8 + 8]) == 1);  // version
    CHECK(raw.compare(body + 256, 20, "Wezt Recording Tools") == 0);       // originator
  }

  // --- NaN/Inf never reach the file as garbage ---
  {
    const auto path = (dir / "nan.wav").string();
    const float bad[4] = {std::nanf(""), INFINITY, -INFINITY, 0.25f};
    const float* pp[1] = {bad};
    for (auto type : {WavSampleType::Int16, WavSampleType::Int24}) {
      WavWriter w;
      CHECK(w.open(path, 1, 48000, type, &err));
      CHECK(w.write(pp, 4));
      CHECK(w.finalize(&err));
      WavReader r;
      CHECK(r.open(path, &err));
      float out[4] = {9, 9, 9, 9};
      float* op[1] = {out};
      CHECK(r.read(0, 4, op) == 4);
      CHECK(out[0] == 0.f && out[1] > 0.99f && out[2] < -0.99f && std::fabs(out[3] - 0.25f) < 1e-3f);
    }
  }

  // --- corrupt headers are rejected, not misread ---
  {
    const auto path = (dir / "badalign.wav").string();
    CHECK(writeWavFloat(path, AudioBuffer(2, 100), 44100));
    std::string raw = slurp(path);
    const auto fmt = raw.find("fmt ");
    raw[fmt + 8 + 12] = 1; raw[fmt + 8 + 13] = 0;  // blockAlign = 1 for 2 x 32-bit channels
    std::ofstream(path, std::ios::binary).write(raw.data(), static_cast<std::streamsize>(raw.size()));
    WavReader r;
    CHECK(!r.open(path, &err));
  }
  // An empty data chunk followed by another chunk is empty, not "unfinalized".
  {
    const auto path = (dir / "emptydata.wav").string();
    CHECK(writeWavFloat(path, AudioBuffer(2, 0), 44100));
    std::string raw = slurp(path);
    raw += "LIST"; raw += std::string("\x04\0\0\0", 4); raw += "INFO";
    std::ofstream(path, std::ios::binary).write(raw.data(), static_cast<std::streamsize>(raw.size()));
    WavReader r;
    CHECK(r.open(path, &err) && r.info().frames == 0);
  }

  // --- clipping: out-of-range input saturates, never wraps ---
  {
    const auto path = (dir / "clip.wav").string();
    std::vector<float> v = {2.f, -2.f, 0.f};
    const float* p[1] = {v.data()};
    WavWriter w;
    CHECK(w.open(path, 1, 48000, WavSampleType::Int16, &err));
    CHECK(w.write(p, 3) && w.finalize(&err));
    WavReader r;
    CHECK(r.open(path, &err));
    float out[3];
    float* op[1] = {out};
    CHECK(r.read(0, 3, op) == 3);
    CHECK(out[0] > 0.999f && out[1] == -1.f && out[2] == 0.f);
  }

  // --- RF64: forced early so it is testable without a 4 GB file ---
  {
    const auto path = (dir / "rf64.wav").string();
    auto sig = makeSignal(2, 3000);
    auto p = ptrs(sig);
    WavWriter w;
    w.setRf64ThresholdBytes(1000);
    CHECK(w.open(path, 2, 48000, WavSampleType::Float32, &err));
    CHECK(w.write(p.data(), 3000) && w.finalize(&err));
    const std::string raw = slurp(path);
    CHECK(raw.compare(0, 4, "RF64") == 0);
    CHECK(le32(raw, 4) == 0xFFFFFFFFu);
    CHECK(raw.compare(12, 4, "ds64") == 0);
    CHECK(le32(raw, 20 + 8) == 3000u * 2 * 4);  // ds64 dataSize (low word)
    WavReader r;
    CHECK(r.open(path, &err));
    CHECK(r.info().rf64 && r.info().frames == 3000);
    std::vector<float> a(3000), b(3000);
    float* op[2] = {a.data(), b.data()};
    CHECK(r.read(0, 3000, op) == 3000);
    CHECK(std::fabs(a[1500] - sig[0][1500]) == 0.f && std::fabs(b[2999] - sig[1][2999]) == 0.f);
  }

  // --- crash recovery: a copy taken mid-recording is readable up to the last header patch ---
  {
    const auto path = (dir / "live.wav").string();
    const auto snap = (dir / "snap.wav").string();
    auto sig = makeSignal(1, 2500);
    auto p = ptrs(sig);
    WavWriter w;
    CHECK(w.open(path, 1, 1000, WavSampleType::Float32, &err));  // 1000 Hz so a patch happens every 1000 frames
    CHECK(w.write(p.data(), 2500));
    fs::copy_file(path, snap, fs::copy_options::overwrite_existing);  // the "crash": writer never finalizes
    WavReader r;
    CHECK(r.open(snap, &err));
    CHECK(r.info().frames >= 2000 && r.info().frames <= 2500);
    w.finalize(&err);
    // And a file whose header says size 0 (never patched) still opens, running to end of file.
    std::string raw = slurp(path);
    const auto d = raw.find("data");
    for (int i = 0; i < 4; ++i) raw[d + 4 + i] = 0;
    std::ofstream(snap, std::ios::binary | std::ios::trunc).write(raw.data(), static_cast<std::streamsize>(raw.size()));
    WavReader r2;
    CHECK(r2.open(snap, &err));
    CHECK(r2.info().frames == 2500);
  }

  // --- bad input fails cleanly ---
  {
    WavReader r;
    CHECK(!r.open((dir / "missing.wav").string(), &err));
    std::ofstream(dir / "junk.wav") << "this is not a wav file at all";
    CHECK(!r.open((dir / "junk.wav").string(), &err));
    CHECK(!WavWriter().open("/nonexistent_dir/x.wav", 2, 48000, WavSampleType::Float32, &err));
  }

  fs::remove_all(dir);
  std::printf(failures ? "FAILED\n" : "wav tests passed\n");
  return failures;
}
