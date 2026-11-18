// Splits a recording of many separate sounds into takes, prints what each one measures like, and can build a
// voice bank from the takes you name.
//   wezt_voice_takes recording.wav
//   wezt_voice_takes recording.wav --build bank.wvb --owner "Your Name" --i-confirm-this-is-my-voice \
//        --use "0=s,1=t,2=m,3=l,4=eh,5=ah,6=oh" [--derive] [--short-vowels] [--base earlier_bank.wvb]
// Names: vowels ah ee eh ae ih oh oo uh er (a held vowel); consonants (a take like "tah": the sound before the
// vowel is cut out); and for a hiss take (no pitch) "s", "sh", "f", "h", "th" (the steady middle is kept).
// "name@tail" cuts a consonant said after its vowel ("1=f@tail" for "ef").
// Your own list of what you said, in order, is more reliable than the guesses in the table.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

#include "io/wav.h"
#include "voice/voice_bank.h"
#include "voice/voice_takes.h"

using namespace wezt; using namespace wezt::voice;

static AudioBuffer slice(const AudioBuffer& a, double sr, double t0, double t1) {
  const std::size_t s = static_cast<std::size_t>(std::max(0.0, t0 - 0.05) * sr), z = std::min(a.frames, static_cast<std::size_t>((t1 + 0.05) * sr));
  AudioBuffer o(1, z > s ? z - s : 0); if (z > s) std::copy(a.channel(0) + s, a.channel(0) + z, o.channel(0)); return o;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("usage: wezt_voice_takes file.wav [--build bank.wvb --owner NAME --i-confirm-this-is-my-voice --use \"0=s,1=t,...\"]\n"); return 2; }
  std::string build, owner, use, base; bool consent = false, derive = false; double minSteady = 0.35;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--build") && i + 1 < argc) build = argv[++i];
    else if (!std::strcmp(argv[i], "--owner") && i + 1 < argc) owner = argv[++i];
    else if (!std::strcmp(argv[i], "--base") && i + 1 < argc) base = argv[++i];
    else if (!std::strcmp(argv[i], "--use") && i + 1 < argc) use = argv[++i];
    else if (!std::strcmp(argv[i], "--i-confirm-this-is-my-voice")) consent = true;
    else if (!std::strcmp(argv[i], "--short-vowels")) minSteady = 0.15;
    else if (!std::strcmp(argv[i], "--derive")) derive = true;
  }
  io::WavReader r; std::string err;
  if (!r.open(argv[1], &err)) { std::printf("cannot read %s: %s\n", argv[1], err.c_str()); return 1; }
  const auto& info = r.info(); AudioBuffer a(info.channels, static_cast<std::size_t>(info.frames));
  std::vector<float*> dst; for (unsigned c = 0; c < info.channels; ++c) dst.push_back(a.channel(c));
  r.read(0, static_cast<std::size_t>(info.frames), dst.data());
  AudioBuffer mono(1, a.frames);
  for (std::size_t i = 0; i < a.frames; ++i) { float s = 0; for (unsigned c = 0; c < info.channels; ++c) s += a.channel(c)[i]; mono.channel(0)[i] = s / static_cast<float>(info.channels); }
  const double sr = info.sampleRate;
  const auto takes = splitTakes(mono, sr);
  std::printf("%zu takes\n idx   from-to (s)   kind          f0     F1    F2  hiss-centre  guess\n", takes.size());
  for (std::size_t i = 0; i < takes.size(); ++i) { const auto& t = takes[i]; std::printf("%4zu  %6.2f-%6.2f  %-12s %5.0f %5.0f %5.0f  %9.0f   %s\n", i, t.from, t.to, t.kind.c_str(), t.f0, t.f1, t.f2, t.hissCentroidHz, t.guess.c_str()); }
  if (build.empty()) return 0;
  if (!consent) { std::printf("A bank can only be built from your own voice or a speaker who agreed: add --i-confirm-this-is-my-voice.\n"); return 1; }
  VoiceBank bank; bank.name = "my voice"; bank.owner = owner; bank.consent = "the caller confirmed the voice is their own or the speaker agreed";
  if (!base.empty() && !loadVoiceBank(base, bank, &err)) { std::printf("cannot read the base bank %s: %s\n", base.c_str(), err.c_str()); return 1; }
  const auto rep = buildBankFromTakes(mono, sr, takes, use, bank, derive, minSteady);
  std::fputs(rep.log.c_str(), stdout); int ok = rep.used, bad = rep.skipped;
  if (!saveVoiceBank(build, bank, &err)) { std::printf("could not save: %s\n", err.c_str()); return 1; }
  std::printf("saved %s: %zu vowels, %zu consonants (%d used, %d skipped)\n", build.c_str(), bank.entries.size(), bank.consonants.size(), ok, bad);
  return 0;
}
