// Measures a voice recording: vibrato, jitter, shimmer, harmonics-to-noise ratio, alpha ratio and
// formants (see src/voice/voice_analysis.h). Run it on a recording of your own voice and on Wezt's
// synthesized singing to compare them number by number.
//   wezt_voice_analyze recording.wav [--from SECONDS --to SECONDS]   (default: the longest steady vowel)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "io/wav.h"
#include "voice/voice_analysis.h"

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("usage: wezt_voice_analyze file.wav [--from s --to s]\n"); return 2; }
  double from = -1, to = -1;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--from")) from = std::atof(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--to")) to = std::atof(argv[i + 1]);
  }
  wezt::io::WavReader r; std::string err;
  if (!r.open(argv[1], &err)) { std::printf("cannot read %s: %s\n", argv[1], err.c_str()); return 1; }
  const auto& info = r.info();
  wezt::AudioBuffer a(info.channels, static_cast<std::size_t>(info.frames));
  std::vector<float*> dst; for (unsigned c = 0; c < info.channels; ++c) dst.push_back(a.channel(c));
  r.read(0, static_cast<std::size_t>(info.frames), dst.data());
  wezt::AudioBuffer mono(1, a.frames);                      // mix to mono
  for (std::size_t i = 0; i < a.frames; ++i) { float s = 0; for (unsigned c = 0; c < info.channels; ++c) s += a.channel(c)[i]; mono.channel(0)[i] = s / static_cast<float>(info.channels); }
  const double sr = info.sampleRate;
  if (from < 0) {
    if (!wezt::voice::findSteadySegment(mono, sr, 0.6, from, to)) { std::printf("No steady held vowel of at least 0.6 s found; give --from and --to.\n"); return 1; }
    std::printf("Using the longest steady stretch, %.2f-%.2f s.\n", from, to);
  }
  std::printf("%s\n", wezt::voice::describeMeasures(wezt::voice::analyzeVoice(mono, sr, from, to)).c_str());
  return 0;
}
