// wavdump <file> [firstN]: prints what Wezt's WAV reader sees. Used to cross-check against other tools.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "io/wav.h"

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("usage: wavdump <file.wav> [firstN]\n"); return 2; }
  wezt::io::WavReader r;
  std::string err;
  if (!r.open(argv[1], &err)) { std::printf("ERROR %s\n", err.c_str()); return 1; }
  const auto& i = r.info();
  std::printf("channels=%u rate=%u frames=%llu bits=%u float=%d rf64=%d bext=%d timeref=%llu desc=%s\n", i.channels, i.sampleRate,
              static_cast<unsigned long long>(i.frames), i.bitsPerSample, i.isFloat, i.rf64, i.hasBext,
              static_cast<unsigned long long>(i.timeReference), i.description.c_str());
  const std::size_t n = argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 0;
  if (n) {
    std::vector<std::vector<float>> b(i.channels, std::vector<float>(n));
    std::vector<float*> p;
    for (auto& v : b) p.push_back(v.data());
    const std::size_t got = r.read(0, n, p.data());
    for (std::size_t f = 0; f < got; ++f) {
      for (unsigned c = 0; c < i.channels; ++c) std::printf("%.9g%c", b[c][f], c + 1 == i.channels ? '\n' : ' ');
    }
  }
  return 0;
}
