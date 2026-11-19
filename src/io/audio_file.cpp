#include "io/audio_file.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>

#include "io/wav.h"

#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "minimp3.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace wezt::io {
namespace {

std::string lowerExt(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const auto sep = path.find_last_of("/\\");
  if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return {};
  std::string e = path.substr(dot + 1);
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

bool readAll(const std::string& path, std::vector<std::uint8_t>& bytes, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { if (error) *error = "cannot open the file"; return false; }
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  if (n <= 0 || n > 200ll * 1024 * 1024) { if (error) *error = n <= 0 ? "the file is empty" : "the file is too large for a start-up sound"; return false; }
  bytes.resize(static_cast<std::size_t>(n));
  f.read(reinterpret_cast<char*>(bytes.data()), n);
  if (!f) { if (error) *error = "cannot read the file"; return false; }
  return true;
}

bool loadMp3(const std::string& path, StereoClip& out, std::string* error, double maxSeconds) {
  std::vector<std::uint8_t> bytes;
  if (!readAll(path, bytes, error)) return false;
  auto dec = std::make_unique<mp3dec_t>();
  mp3dec_init(dec.get());
  std::size_t pos = 0;
  std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
  out = StereoClip{};
  unsigned rate = 0;
  std::size_t maxFrames = 0;
  int guard = 0;
  while (pos < bytes.size() && guard++ < 2000000) {
    mp3dec_frame_info_t info;
    const int samples = mp3dec_decode_frame(dec.get(), bytes.data() + pos, static_cast<int>(std::min<std::size_t>(bytes.size() - pos, 1u << 30)), pcm.data(), &info);
    if (info.frame_bytes <= 0) break;   // nothing that looks like a frame is left
    pos += static_cast<std::size_t>(info.frame_bytes);
    if (samples <= 0 || info.channels <= 0 || info.hz <= 0) continue;   // skipped a tag or a damaged frame
    if (!rate) { rate = static_cast<unsigned>(info.hz); out.rate = rate; maxFrames = static_cast<std::size_t>(maxSeconds * rate); }
    if (static_cast<unsigned>(info.hz) != rate) continue;   // a stream that changes rate: ignore the odd frames
    for (int i = 0; i < samples; ++i) {
      const float* p = pcm.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(info.channels);
      out.l.push_back(p[0]);
      out.r.push_back(info.channels > 1 ? p[1] : p[0]);
    }
    if (out.l.size() >= maxFrames) { out.l.resize(maxFrames); out.r.resize(maxFrames); break; }
  }
  if (out.l.empty()) { if (error) *error = "no MP3 audio found in the file"; return false; }
  return true;
}

bool loadWav(const std::string& path, StereoClip& out, std::string* error, double maxSeconds) {
  WavReader w;
  std::string err;
  if (!w.open(path, &err)) { if (error) *error = err.empty() ? "not a readable WAV file" : err; return false; }
  const WavInfo& in = w.info();
  if (in.channels == 0 || in.sampleRate == 0 || in.frames == 0) { if (error) *error = "the WAV file has no audio"; return false; }
  const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(in.frames, static_cast<std::uint64_t>(maxSeconds * in.sampleRate)));
  std::vector<std::vector<float>> ch(in.channels, std::vector<float>(n));
  std::vector<float*> ptrs;
  for (auto& c : ch) ptrs.push_back(c.data());
  const std::size_t got = w.read(0, n, ptrs.data());
  if (got == 0) { if (error) *error = "the WAV file has no audio"; return false; }
  out = StereoClip{};
  out.rate = in.sampleRate;
  out.l.assign(ch[0].begin(), ch[0].begin() + static_cast<std::ptrdiff_t>(got));
  out.r.assign((in.channels > 1 ? ch[1] : ch[0]).begin(), (in.channels > 1 ? ch[1] : ch[0]).begin() + static_cast<std::ptrdiff_t>(got));
  return true;
}

}  // namespace

bool loadStereoClip(const std::string& path, StereoClip& out, std::string* error, double maxSeconds) {
  const std::string ext = lowerExt(path);
  bool ok;
  if (ext == "mp3") ok = loadMp3(path, out, error, maxSeconds);
  else if (ext == "wav" || ext == "wave" || ext == "bwf") ok = loadWav(path, out, error, maxSeconds);
  else { if (error) *error = "use a WAV or MP3 file"; return false; }
  if (!ok) return false;
  for (auto* v : {&out.l, &out.r})
    for (float& x : *v) if (!std::isfinite(x)) x = 0.f;
  return true;
}

StereoClip resampled(const StereoClip& in, unsigned newRate) {
  if (in.rate == newRate || in.l.empty() || newRate == 0) return in;
  StereoClip o;
  o.rate = newRate;
  const double ratio = static_cast<double>(in.rate) / newRate;
  const std::size_t n = static_cast<std::size_t>(static_cast<double>(in.l.size()) / ratio);
  o.l.resize(n); o.r.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double p = static_cast<double>(i) * ratio;
    const std::size_t a = static_cast<std::size_t>(p);
    const std::size_t b = std::min(a + 1, in.l.size() - 1);
    const float f = static_cast<float>(p - static_cast<double>(a));
    o.l[i] = in.l[a] + (in.l[b] - in.l[a]) * f;
    o.r[i] = in.r[a] + (in.r[b] - in.r[a]) * f;
  }
  return o;
}

}  // namespace wezt::io
