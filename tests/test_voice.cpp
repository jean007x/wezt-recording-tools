#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "voice/voice_model.h"

using namespace wezt;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {
struct Vowel { double f[3]; };
const Vowel kV[5] = {{{700, 1220, 2600}}, {{270, 2300, 3000}}, {{300, 870, 2250}}, {{530, 1840, 2480}}, {{570, 840, 2410}}};

// A synthetic singer: a pulse train through three resonators. `scale` moves the formants (a
// different vocal tract); `bw` is their width.
AudioBuffer sing(const std::vector<std::pair<int, double>>& notes, double sr, double scale, double bw, double noteSec = 0.35) {
  AudioBuffer out(1, static_cast<std::size_t>(sr * noteSec * static_cast<double>(notes.size())));
  double phase = 0, y1[3] = {}, y2[3] = {};
  std::size_t i = 0;
  for (auto& [v, f0] : notes) {
    const std::size_t n = static_cast<std::size_t>(sr * noteSec);
    for (std::size_t k = 0; k < n && i < out.frames; ++k, ++i) {
      phase += f0 / sr; if (phase >= 1.0) phase -= 1.0;
      const double src = (1.0 - 2.0 * phase) * 0.5;
      double s = 0;
      for (int r = 0; r < 3; ++r) {
        const double fc = kV[v].f[r] * scale, rr = std::exp(-M_PI * bw * (1 + r * 0.5) / sr), th = 2 * M_PI * fc / sr;
        const double y = src * (1 - rr) + 2 * rr * std::cos(th) * y1[r] - rr * rr * y2[r];
        y2[r] = y1[r]; y1[r] = y; s += y * (r == 0 ? 1.0 : 0.6);
      }
      const double env = std::min({1.0, k / 300.0, (n - k) / 300.0});
      out.channel(0)[i] = static_cast<float>(0.5 * s * env);
    }
  }
  return out;
}
std::vector<std::pair<int, double>> randomNotes(int count, unsigned seed) {
  std::vector<std::pair<int, double>> v; unsigned s = seed;
  for (int i = 0; i < count; ++i) { s = s * 1664525u + 1013904223u; const int vow = (s >> 8) % 5; s = s * 1664525u + 1013904223u; v.push_back({vow, 150.0 + static_cast<double>((s >> 8) % 120)}); }
  return v;
}
double cepDist(const std::vector<voice::VoiceFrame>& a, const std::vector<voice::VoiceFrame>& b) {
  double s = 0; int n = 0;
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
    if (!a[i].voiced || !b[i].voiced) continue;
    double d = 0; for (int k = 1; k < voice::kCep; ++k) { const double e = a[i].cep[k] - b[i].cep[k]; d += e * e; }
    s += std::sqrt(d); ++n;
  }
  return n ? s / n : 1e9;
}
double medianF0(const std::vector<voice::VoiceFrame>& f) {
  std::vector<double> v; for (auto& x : f) if (x.voiced) v.push_back(x.f0);
  if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2];
}
}  // namespace

int main() {
  const double sr = 22050;
  // training material: voice B, 50 s
  std::vector<AudioBuffer> train;
  train.push_back(sing(randomNotes(70, 1), sr, 1.3, 90));
  train.push_back(sing(randomNotes(70, 2), sr, 1.3, 90));
  voice::TrainParams tp; tp.name = "bee"; tp.owner = "tester"; tp.codes = 48;
  auto bad = voice::trainVoiceModel(train, sr, tp);      // no consent
  CHECK(!bad.ok);
  tp.consent = true;
  auto shortRes = voice::trainVoiceModel({sing(randomNotes(10, 3), sr, 1.3, 90)}, sr, tp);
  CHECK(!shortRes.ok);                                   // 3.5 s is not enough
  auto tr = voice::trainVoiceModel(train, sr, tp);
  std::printf("%s\n", tr.message.c_str());
  CHECK(tr.ok);
  CHECK(tr.model.codes > 8 && tr.model.medianF0 > 140 && tr.model.medianF0 < 280);

  // file round trip
  const std::string path = (std::filesystem::temp_directory_path() / "wezt_test.wvm").string();
  CHECK(voice::saveVoiceModel(path, tr.model));
  voice::VoiceModel loaded;
  CHECK(voice::loadVoiceModel(path, loaded));
  CHECK(loaded.name == "bee" && loaded.owner == "tester" && loaded.voicedBook == tr.model.voicedBook);
  { FILE* f = std::fopen(path.c_str(), "wb"); std::fputs("junk", f); std::fclose(f); }
  voice::VoiceModel junk; std::string err;
  CHECK(!voice::loadVoiceModel(path, junk, &err));
  CHECK(!voice::loadVoiceModel("/no/such/file.wvm", junk, &err));

  // conversion: voice A (different vocal tract) sings a phrase; B sings the same phrase as the reference
  const auto phrase = randomNotes(14, 99);
  const AudioBuffer a = sing(phrase, sr, 1.0, 70), bref = sing(phrase, sr, 1.3, 90);
  voice::ConvertParams cp;
  const AudioBuffer c = voice::convertVoice(a, sr, loaded, cp);
  CHECK(c.frames == a.frames && c.peak() > 0.02f && c.peak() <= 1.0f);
  const auto fa = voice::analyzeVoice(a, sr), fb = voice::analyzeVoice(bref, sr), fc = voice::analyzeVoice(c, sr);
  const double before = cepDist(fa, fb), after = cepDist(fc, fb);
  std::printf("distance to the target voice: before %.2f after %.2f\n", before, after);
  { voice::ConvertParams q; for (float w : {0.f, 0.6f, 1.f}) { q.codebookWeight = w; std::printf("w=%.1f A->B %.2f   B->B (floor) %.2f\n", w, cepDist(voice::analyzeVoice(voice::convertVoice(a, sr, loaded, q), sr), fb), cepDist(voice::analyzeVoice(voice::convertVoice(bref, sr, loaded, q), sr), fb)); } }
  CHECK(after < before * 0.6);                           // clearly closer to the trained voice
  const double fa0 = medianF0(fa), fc0 = medianF0(fc);
  CHECK(std::fabs(fc0 - fa0) / fa0 < 0.04);               // the sung pitch is kept
  // a model must not be usable with an empty input
  CHECK(voice::convertVoice(AudioBuffer(), sr, loaded, cp).frames == 0);
  std::printf(failures ? "FAILED\n" : "voice tests passed\n");
  return failures;
}
