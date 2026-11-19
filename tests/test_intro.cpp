// The start-up intro's non-visual parts: settings file, WAV/MP3 loading, the synthesized sound, the logo picture.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "io/audio_file.h"
#include "io/wav.h"
#include "ui/intro_sound.h"
#include "ui/logo.h"
#include "util/prefs.h"

using namespace wezt;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static double rms(const io::StereoClip& c, double from, double to) {
  const std::size_t a = static_cast<std::size_t>(from * c.rate), b = std::min(c.frames(), static_cast<std::size_t>(to * c.rate));
  double s = 0; for (std::size_t i = a; i < b; ++i) s += c.l[i] * c.l[i] + c.r[i] * c.r[i];
  return b > a ? std::sqrt(s / (2.0 * static_cast<double>(b - a))) : 0.0;
}

static void testPrefs() {
  const fs::path dir = fs::temp_directory_path() / "wezt_prefs_test";
  fs::remove_all(dir);
  const std::string file = (dir / "sub" / "settings.txt").string();
  Prefs d = loadPrefs(file);                                  // no file: defaults
  CHECK(d.introEnabled && d.introSoundPath.empty());
  Prefs p; p.introEnabled = false; p.introSoundPath = "/music/my intro = v2.mp3";
  CHECK(savePrefs(p, file));                                  // creates the folder
  Prefs q = loadPrefs(file);
  CHECK(!q.introEnabled && q.introSoundPath == "/music/my intro = v2.mp3");
  p.introEnabled = true; p.introSoundPath.clear();
  CHECK(savePrefs(p, file));
  q = loadPrefs(file);
  CHECK(q.introEnabled && q.introSoundPath.empty());
  { std::ofstream f(file, std::ios::trunc); f << "garbage\nintro=0\r\nunknown=7\n"; }   // unknown keys, CRLF, a junk line
  q = loadPrefs(file);
  CHECK(!q.introEnabled);
  CHECK(!savePrefs(p, ""));
  CHECK(loadPrefs("").introEnabled);
#if defined(_WIN32)
  _putenv_s("WEZT_CONFIG_DIR", dir.string().c_str());
#else
  setenv("WEZT_CONFIG_DIR", dir.string().c_str(), 1);
#endif
  CHECK(prefsPath() == dir.string() + "/settings.txt");
  fs::remove_all(dir);
}

static void testAudioFiles() {
  const fs::path dir = fs::temp_directory_path() / "wezt_intro_audio";
  fs::remove_all(dir); fs::create_directories(dir);
  {  // a mono 22.05 kHz WAV, 0.5 s of 440 Hz
    const std::string path = (dir / "tone.wav").string();
    io::WavWriter w; std::string err;
    CHECK(w.open(path, 1, 22050, io::WavSampleType::Int16, &err));
    std::vector<float> x(11025);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * std::sin(2.f * 3.14159265f * 440.f * static_cast<float>(i) / 22050.f);
    const float* p = x.data();
    CHECK(w.write(&p, x.size()));
    CHECK(w.finalize(&err));
    io::StereoClip c;
    CHECK(io::loadStereoClip(path, c, &err));
    CHECK(c.rate == 22050 && c.frames() == 11025 && c.l == c.r);   // mono became dual mono
    CHECK(std::fabs(rms(c, 0, 0.5) - 0.5 / std::sqrt(2.0)) < 0.01);
    io::StereoClip cut;
    CHECK(io::loadStereoClip(path, cut, &err, 0.1) && cut.frames() == 2205);   // the length limit
    const io::StereoClip up = io::resampled(c, 48000);
    CHECK(up.rate == 48000 && std::abs(static_cast<double>(up.frames()) - 24000.0) <= 1.0);
    CHECK(std::fabs(rms(up, 0, 0.5) - rms(c, 0, 0.5)) < 0.01);
  }
  {  // a real MP3 (1 s, 440 Hz, 44.1 kHz stereo, made with ffmpeg/LAME)
    io::StereoClip c; std::string err;
    CHECK(io::loadStereoClip(std::string(WEZT_TEST_DATA_DIR) + "/tone440.mp3", c, &err));
    CHECK(c.rate == 44100);
    CHECK(c.seconds() > 0.95 && c.seconds() < 1.15);
    CHECK(rms(c, 0.2, 0.8) > 0.05);                       // it decoded to sound, not silence
    int crossings = 0;                                     // and the pitch is right: ~440 rising zero crossings per second
    for (std::size_t i = 22050; i + 1 < 22050 + 44100 / 2 && i + 1 < c.frames(); ++i) if (c.l[i] < 0 && c.l[i + 1] >= 0) ++crossings;
    CHECK(crossings >= 215 && crossings <= 225);
  }
  {  // failures give a reason and no crash
    io::StereoClip c; std::string err;
    CHECK(!io::loadStereoClip((dir / "missing.wav").string(), c, &err) && !err.empty());
    err.clear();
    CHECK(!io::loadStereoClip((dir / "x.ogg").string(), c, &err) && err.find("WAV or MP3") != std::string::npos);
    { std::ofstream f(dir / "junk.mp3", std::ios::binary); for (int i = 0; i < 5000; ++i) f.put(static_cast<char>((i * 37) & 0x7f)); }
    err.clear();
    CHECK(!io::loadStereoClip((dir / "junk.mp3").string(), c, &err) && !err.empty());
    { std::ofstream f(dir / "junk.wav", std::ios::binary); f << "this is not a wav file"; }
    err.clear();
    CHECK(!io::loadStereoClip((dir / "junk.wav").string(), c, &err) && !err.empty());
    { std::ofstream f(dir / "empty.mp3", std::ios::binary); }
    CHECK(!io::loadStereoClip((dir / "empty.mp3").string(), c, &err));
  }
  fs::remove_all(dir);
}

static void testSound() {
  const io::StereoClip a = ui::renderIntroSound(48000), b = ui::renderIntroSound(48000);
  CHECK(a.rate == 48000 && a.l == b.l && a.r == b.r);                 // same samples every time
  CHECK(a.seconds() > 9.0 && a.seconds() < 9.5);
  float peak = 0; bool finite = true;
  for (std::size_t i = 0; i < a.frames(); ++i) { peak = std::max({peak, std::fabs(a.l[i]), std::fabs(a.r[i])}); finite = finite && std::isfinite(a.l[i]) && std::isfinite(a.r[i]); }
  CHECK(finite && peak > 0.8f && peak <= 0.9f);                       // loud but never clipping
  CHECK(rms(a, 0, 0.01) < 0.002);                                     // starts from silence (no click)
  const double early = rms(a, 0.5, 1.0), rising = rms(a, 3.3, 3.75);
  CHECK(rising > early * 2.0);                                        // the build-up really builds
  CHECK(rms(a, 3.8, 4.0) > rising);                                   // the landing is the biggest thing so far
  const double clash = rms(a, 6.0, 6.3), before = rms(a, 5.8, 6.0);
  CHECK(clash > 0.12 && clash > before);                              // the clash hits at the opening
  CHECK(rms(a, 8.2, 8.8) < clash * 0.4);                             // and rings out
  CHECK(rms(a, a.seconds() - 0.05, a.seconds()) < 0.002);             // ends silent
  const io::StereoClip c = ui::renderIntroSound(44100);
  CHECK(c.rate == 44100 && std::fabs(c.seconds() - a.seconds()) < 0.01);
#if !defined(_WIN32) && !defined(__APPLE__)
  ui::IntroPlayer pl; std::string m;
  CHECK(!pl.start("", &m) && !m.empty() && !pl.playing());            // no audio backend here: a clear "no", nothing breaks
  pl.stop();
#endif
}

static void testLogo() {
  const ui::LogoImage g = ui::makeGoldLogo(), w = ui::makeWhiteLogo();
  CHECK(g.w == 680 && g.h == 256 && w.w == g.w && w.h == g.h);
  CHECK(g.rgba.size() == static_cast<std::size_t>(g.w * g.h) * 4);
  double cover = 0, r = 0, gr = 0, bl = 0; std::size_t solid = 0;
  for (std::size_t i = 0; i < static_cast<std::size_t>(g.w * g.h); ++i) {
    cover += g.rgba[i * 4 + 3] / 255.0;
    CHECK(w.rgba[i * 4 + 3] <= g.rgba[i * 4 + 3]);                    // the white glint picture is the shape only (the gold one adds the soft shadow)
    if (g.rgba[i * 4 + 3] > 250) { ++solid; r += g.rgba[i * 4]; gr += g.rgba[i * 4 + 1]; bl += g.rgba[i * 4 + 2]; }
  }
  cover /= g.w * g.h;
  CHECK(cover > 0.05 && cover < 0.5);                                 // a line drawing, not a blank or a filled box
  CHECK(solid > 1000);
  r /= static_cast<double>(solid); gr /= static_cast<double>(solid); bl /= static_cast<double>(solid);
  CHECK(r > gr && gr > bl && r > 150 && bl < 140);                    // it is gold: red > green > blue, and bright enough
  CHECK(g.rgba[3] == 0);                                              // the corner is empty
  const auto m = ui::mipChain(g);
  CHECK(m.size() == 10 && m.front().w == 680 && m.back().w == 1 && m.back().h == 1);
  bool ok = true;
  for (std::size_t i = 1; i < m.size(); ++i) ok = ok && m[i].w >= 1 && m[i].rgba.size() == static_cast<std::size_t>(m[i].w * m[i].h) * 4;
  CHECK(ok);
}

int main() {
  testPrefs();
  testAudioFiles();
  testSound();
  testLogo();
  std::printf(failures ? "FAILED\n" : "intro tests passed\n");
  return failures;
}
