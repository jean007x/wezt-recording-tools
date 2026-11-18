#include "music/separation.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>

#include "ai/audio_file.h"
#include "ai/fft.h"

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wezt::music {
namespace {

using Cf = std::complex<float>;
constexpr std::size_t kN = 2048, kHop = 512, kBins = kN / 2 + 1;
constexpr double kPi = 3.14159265358979323846;

float smooth(float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); }

struct Spec { std::size_t frames = 0; std::vector<Cf> d; Cf& at(std::size_t t, std::size_t k) { return d[t * kBins + k]; } const Cf& at(std::size_t t, std::size_t k) const { return d[t * kBins + k]; } };

Spec stft(const std::vector<float>& x, std::size_t frames, const ai::FFT& fft, const std::vector<double>& win) {
  Spec s; s.frames = frames; s.d.assign(frames * kBins, Cf());
  std::vector<std::complex<double>> buf(kN);
  for (std::size_t t = 0; t < frames; ++t) {
    for (std::size_t i = 0; i < kN; ++i) { const std::size_t p = t * kHop + i; buf[i] = (p < x.size() ? x[p] : 0.f) * win[i]; }
    fft.forward(buf);
    for (std::size_t k = 0; k < kBins; ++k) s.at(t, k) = Cf(static_cast<float>(buf[k].real()), static_cast<float>(buf[k].imag()));
  }
  return s;
}

std::vector<float> istft(const Spec& s, std::size_t length, const ai::FFT& fft, const std::vector<double>& win) {
  std::vector<double> acc(length + kN * 2, 0.0), norm(length + kN * 2, 0.0);
  std::vector<std::complex<double>> buf(kN);
  for (std::size_t t = 0; t < s.frames; ++t) {
    for (std::size_t k = 0; k < kBins; ++k) buf[k] = std::conj(std::complex<double>(s.at(t, k)));   // conj for the inverse trick
    for (std::size_t k = kBins; k < kN; ++k) buf[k] = std::conj(std::conj(std::complex<double>(s.at(t, kN - k))));
    fft.forward(buf);
    for (std::size_t i = 0; i < kN; ++i) {
      const std::size_t p = t * kHop + i;
      acc[p] += std::conj(buf[i]).real() / static_cast<double>(kN) * win[i];
      norm[p] += win[i] * win[i];
    }
  }
  std::vector<float> out(length);
  for (std::size_t i = 0; i < length; ++i) out[i] = norm[i] > 1e-9 ? static_cast<float>(acc[i] / norm[i]) : 0.f;
  return out;
}

}  // namespace

bool separateDsp(const AudioBuffer& mix, double sr, StemSet& out, std::string* err, const SeparationOptions& opt) {
  auto fail = [&](const char* m) { if (err) *err = m; return false; };
  if (mix.channels == 0 || mix.channels > 2 || mix.frames < kN) return fail("need a mono or stereo file of at least a fraction of a second");
  if (static_cast<double>(mix.frames) / sr > opt.maxSeconds) return fail("file is longer than the separation limit");
  const std::size_t N = mix.frames;
  const std::vector<float> L(mix.channel(0), mix.channel(0) + N), R(mix.channel(mix.channels > 1 ? 1 : 0), mix.channel(mix.channels > 1 ? 1 : 0) + N);
  const std::size_t frames = (N + kN) / kHop + 1;
  ai::FFT fft(kN);
  std::vector<double> win(kN);
  for (std::size_t i = 0; i < kN; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kN));
  Spec SL = stft(L, frames, fft, win), SR = stft(R, frames, fft, win);

  // magnitude of the mid signal
  std::vector<float> mag(frames * kBins);
  for (std::size_t i = 0; i < mag.size(); ++i) mag[i] = std::abs(0.5f * (SL.d[i] + SR.d[i]));
  // harmonic: median across time; percussive: median across frequency
  const int half = std::max(1, opt.medianWidth / 2);
  std::vector<float> H(mag.size()), P(mag.size());
  {
    std::vector<float> w;
    for (std::size_t k = 0; k < kBins; ++k)
      for (std::size_t t = 0; t < frames; ++t) {
        w.clear();
        for (int d = -half; d <= half; ++d) { const long tt = static_cast<long>(t) + d; if (tt >= 0 && tt < static_cast<long>(frames)) w.push_back(mag[static_cast<std::size_t>(tt) * kBins + k]); }
        std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
        H[t * kBins + k] = w[w.size() / 2];
      }
    for (std::size_t t = 0; t < frames; ++t)
      for (std::size_t k = 0; k < kBins; ++k) {
        w.clear();
        for (int d = -half; d <= half; ++d) { const long kk = static_cast<long>(k) + d; if (kk >= 0 && kk < static_cast<long>(kBins)) w.push_back(mag[t * kBins + static_cast<std::size_t>(kk)]); }
        std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
        P[t * kBins + k] = w[w.size() / 2];
      }
  }
  Spec dr = SL, ba = SL, vo = SL, ot = SL, drR = SR, baR = SR, voR = SR, otR = SR;
  const float binHz = static_cast<float>(sr / static_cast<double>(kN));
  for (std::size_t t = 0; t < frames; ++t)
    for (std::size_t k = 0; k < kBins; ++k) {
      const std::size_t i = t * kBins + k;
      const float h2 = H[i] * H[i], p2 = P[i] * P[i];
      const float mp = p2 / (h2 + p2 + 1e-12f), mh = 1.f - mp;
      const float f = static_cast<float>(k) * binHz;
      const float b = 1.f - smooth((f - static_cast<float>(opt.bassCutoffHz) * 0.75f) / (static_cast<float>(opt.bassCutoffHz) * 0.5f));
      const float l = std::abs(SL.d[i]), r = std::abs(SR.d[i]);
      float centre = 0.f;
      if (l + r > 1e-9f) {
        const float ratio = std::min(l, r) / std::max(std::max(l, r), 1e-12f);
        const float cosPhase = l > 0 && r > 0 ? std::real(SL.d[i] * std::conj(SR.d[i])) / (l * r) : 0.f;
        centre = ratio * ratio * std::max(0.f, cosPhase);
      }
      const float band = smooth((f - 120.f) / 80.f) * (1.f - 0.7f * smooth((f - 6000.f) / 4000.f));
      const float mb = mh * b, mv = mh * (1.f - b) * centre * band, mo = mh * (1.f - b) - mv;
      dr.d[i] = SL.d[i] * mp;  drR.d[i] = SR.d[i] * mp;
      ba.d[i] = SL.d[i] * mb;  baR.d[i] = SR.d[i] * mb;
      vo.d[i] = SL.d[i] * mv;  voR.d[i] = SR.d[i] * mv;
      ot.d[i] = SL.d[i] * mo;  otR.d[i] = SR.d[i] * mo;
    }
  auto make = [&](const Spec& a, const Spec& b, AudioBuffer& dst) {
    dst = AudioBuffer(2, N);
    const auto l = istft(a, N + kN, fft, win), r = istft(b, N + kN, fft, win);
    // the analysis frames were taken from sample 0 without padding, so there is no offset to remove
    std::copy(l.begin(), l.begin() + static_cast<long>(N), dst.channel(0));
    std::copy(r.begin(), r.begin() + static_cast<long>(N), dst.channel(1));
  };
  make(dr, drR, out.drums); make(ba, baR, out.bass); make(vo, voR, out.vocals); make(ot, otR, out.other);
  return true;
}

bool externalSeparatorAvailable(std::string* which) {
#if defined(_WIN32)
  (void)which;
  return false;
#else
  const char* path = std::getenv("PATH");
  if (!path) return false;
  std::string p = path, item;
  for (std::size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == ':') {
      if (!item.empty()) {
        const auto cand = std::filesystem::path(item) / "demucs";
        std::error_code ec;
        if (std::filesystem::is_regular_file(cand, ec) && access(cand.c_str(), X_OK) == 0) { if (which) *which = cand.string(); return true; }
      }
      item.clear();
    } else item += p[i];
  }
  return false;
#endif
}

bool separateExternal(const std::string& wav, const std::string& workDir, StemSet& out, double& rate, std::string* err) {
#if defined(_WIN32)
  (void)wav; (void)workDir; (void)out; (void)rate;
  if (err) *err = "external separators are not wired up on this platform yet";
  return false;
#else
  namespace fs = std::filesystem;
  auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
  std::string exe;
  if (!externalSeparatorAvailable(&exe)) return fail("no neural separator (demucs) found on the PATH");
  std::error_code ec;
  fs::create_directories(workDir, ec);
  const pid_t pid = fork();
  if (pid < 0) return fail("cannot start the separator");
  if (pid == 0) {
    execl(exe.c_str(), exe.c_str(), "-o", workDir.c_str(), wav.c_str(), static_cast<char*>(nullptr));  // no shell involved
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return fail("lost the separator process"); }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return fail("the separator failed");
  // the output folder is <workDir>/<model>/<track name>/{drums,bass,vocals,other}.wav
  const std::string stem = fs::path(wav).stem().string();
  for (auto& model : fs::directory_iterator(workDir, ec)) {
    const fs::path d = model.path() / stem;
    if (!fs::exists(d / "vocals.wav", ec)) continue;
    double r1 = 0;
    std::string e;
    AudioBuffer* dst[4] = {&out.drums, &out.bass, &out.vocals, &out.other};
    const char* names[4] = {"drums.wav", "bass.wav", "vocals.wav", "other.wav"};
    for (int i = 0; i < 4; ++i) if (!ai::loadAudioFile((d / names[i]).string(), *dst[i], r1, &e)) return fail("cannot read " + std::string(names[i]) + ": " + e);
    rate = r1;
    return true;
  }
  return fail("the separator finished but its stems were not found");
#endif
}

}  // namespace wezt::music
