#include "ai/audio_file.h"

#include <algorithm>

#include "io/wav.h"

namespace wezt::ai {

bool loadAudioFile(const std::string& path, AudioBuffer& out, double& sr, std::string* error, double maxSeconds) {
  io::WavReader r;
  std::string e;
  if (!r.open(path, &e)) { if (error) *error = e.empty() ? "cannot open " + path : e; return false; }
  const auto& info = r.info();
  if (info.channels == 0 || info.sampleRate < 8000 || info.frames == 0) { if (error) *error = "the file has no usable audio"; return false; }
  const std::uint64_t cap = static_cast<std::uint64_t>(maxSeconds * info.sampleRate);
  const std::size_t frames = static_cast<std::size_t>(std::min<std::uint64_t>(info.frames, cap));
  const unsigned ch = std::min(info.channels, 2u);
  AudioBuffer all(info.channels, frames);
  std::vector<float*> dst(info.channels);
  for (unsigned c = 0; c < info.channels; ++c) dst[c] = all.channel(c);
  const std::size_t got = r.read(0, frames, dst.data());
  if (got == 0) { if (error) *error = "the file could not be read"; return false; }
  out = AudioBuffer(ch, got);
  for (unsigned c = 0; c < ch; ++c) std::copy(all.channel(c), all.channel(c) + got, out.channel(c));
  sr = info.sampleRate;
  return true;
}

}  // namespace wezt::ai
