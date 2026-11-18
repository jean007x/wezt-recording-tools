#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

#include "audio/audio_device.h"
#include "engine/engine.h"
#include "engine/graph.h"
#include "io/wav_writer.h"
#include "audio/sample_convert.h"
#include "plugins/aax_twin.h"
#include "plugins/plugin_library.h"

using namespace wezt;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static float peakOf(const AudioBuffer& b) {
  float p = 0;
  for (float v : b.data) p = std::fmax(p, std::fabs(v));
  return p;
}

static void testGraph() {
  Graph g;
  auto s = g.add(std::make_unique<Sine>(1000.0));
  auto gain = g.add(std::make_unique<Gain>(0.5f));
  CHECK(g.connect(s, gain));
  CHECK(!g.connect(gain, s));  // cycle rejected
  g.setOutput(gain);
  g.prepare(48000, 2, 480);
  AudioBuffer out;
  g.render(out);
  CHECK(peakOf(out) > 0.45f && peakOf(out) <= 0.5f + 1e-4f);
  CHECK(io::writeWavFloat("test_out.wav", out, 48000, "wezt test", 0));
  CHECK(fs::file_size("test_out.wav") > 44 + 602);
}

static void testTransportAndEngine() {
  Graph g;
  auto s = g.add(std::make_unique<Sine>(1000.0));
  g.setOutput(s);
  Engine engine(g, 128, 2, 48000.0, 2);

  audio::NullDevice dev;
  audio::AudioConfig cfg;
  cfg.bufferFrames = 128;
  std::string err;
  CHECK(dev.open(cfg, &engine, &err));
  CHECK(dev.start(&err));

  // Stopped transport: silence, position stays at 0, even with odd callback sizes.
  dev.pump(4, 100);
  CHECK(engine.transport().position() == 0);

  engine.transport().play();
  dev.pump(10, 100);  // 1000 frames in odd 100-frame callbacks: the engine is sample-accurate
  CHECK(engine.transport().position() == 1000);

  engine.transport().stop();
  const auto stoppedAt = engine.transport().position();
  dev.pump(8, 128);
  CHECK(engine.transport().position() == stoppedAt);

  // Locate and loop.
  engine.transport().locate(5000);
  engine.transport().play();
  dev.pump(2, 128);
  CHECK(engine.transport().position() >= 5000);
  engine.transport().stop();
  engine.transport().setLoop(1000, 1256);  // two blocks
  engine.transport().locate(1000);
  engine.transport().play();
  dev.pump(20, 128);
  const auto p = engine.transport().position();
  CHECK(p >= 1000 && p < 1256);
}

static void testRecording() {
  Graph g;
  auto gain = g.add(std::make_unique<Gain>(1.0f));
  g.setOutput(gain);
  Engine engine(g, 64, 2, 48000.0, 2);
  audio::NullDevice dev;
  audio::AudioConfig cfg;
  cfg.bufferFrames = 64;
  std::string err;
  CHECK(dev.open(cfg, &engine, &err));
  CHECK(dev.start(&err));

  dev.pump(2, 64, 0.25f);  // not recording: nothing captured
  CHECK(engine.recordedFrames() == 0);

  engine.transport().record();
  dev.pump(5, 64, 0.25f);
  engine.transport().stop();
  dev.pump(3, 64, 0.25f);
  CHECK(engine.recordedFrames() == 5 * 64);
  AudioBuffer rec = engine.takeRecording();
  CHECK(rec.frames == 5 * 64 && rec.channels == 2);
  CHECK(std::fabs(rec.channel(0)[10] - 0.25f) < 1e-6f && std::fabs(rec.channel(1)[300] - 0.25f) < 1e-6f);
  CHECK(io::writeWavFloat("test_rec.wav", rec, 48000, "recorded take", 0));
}

static void testAaxTwins() {
  using namespace plugins;
  const fs::path root = fs::temp_directory_path() / "wezt_aax_twin_test";
  fs::remove_all(root);
  fs::create_directories(root / "aax");
  fs::create_directories(root / "vst3");
  std::ofstream(root / "aax" / "Studio EQ.aaxplugin").put('x');
  std::ofstream(root / "aax" / "Only In ProTools.aaxplugin").put('x');
  std::ofstream(root / "aax" / "Big_Comp (AAX).aaxplugin").put('x');
  fs::create_directories(root / "vst3" / "Studio EQ.vst3");  // bundle on macOS
  std::ofstream(root / "vst3" / "big_comp.vst3").put('x');
  std::ofstream(root / "vst3" / "Studio EQ.clap").put('x');

  auto aax = findAaxPlugins({(root / "aax").string()});
  auto host = findHostablePlugins({(root / "vst3").string()});
  CHECK(aax.size() == 3);
  CHECK(host.size() == 3);
  auto matches = resolveTwins(aax, host);
  int withTwin = 0;
  for (auto& m : matches) {
    if (m.aax.name == "Studio EQ") { CHECK(m.twin && m.twin->format == Format::VST3); }  // VST3 beats CLAP
    if (m.aax.name == "Only In ProTools") CHECK(!m.twin);
    if (m.aax.name == "Big_Comp (AAX)") CHECK(m.twin && m.twin->name == "big_comp");
    if (m.twin) ++withTwin;
  }
  CHECK(withTwin == 2);
  CHECK(normalizePluginName("Foo-Bar (AAX) x64") == "foobar");
  fs::remove_all(root);
}

// Minimal fake executables: just the header fields the bitness detector reads.
static void writePE(const fs::path& p, unsigned machine) {
  std::string b(0x100, '\0');
  b[0] = 'M'; b[1] = 'Z';
  b[0x3C] = static_cast<char>(0x80);       // e_lfanew = 0x80
  b[0x80] = 'P'; b[0x81] = 'E';
  b[0x84] = static_cast<char>(machine & 0xFF);
  b[0x85] = static_cast<char>(machine >> 8);
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(b.data(), static_cast<std::streamsize>(b.size()));
}

static void testPluginLibrary() {
  using namespace plugins;
  const fs::path root = fs::temp_directory_path() / "wezt_library_test";
  fs::remove_all(root);

  // --- bitness from file headers ---
  writePE(root / "bits" / "a32.dll", 0x014C);
  writePE(root / "bits" / "a64.dll", 0x8664);
  CHECK(detectBitness((root / "bits" / "a32.dll").string()) == 32);
  CHECK(detectBitness((root / "bits" / "a64.dll").string()) == 64);
  { std::ofstream f(root / "bits" / "e.so", std::ios::binary); const char elf[8] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0}; f.write(elf, 8); }
  CHECK(detectBitness((root / "bits" / "e.so").string()) == 64);
  fs::create_directories(root / "bits" / "x.vst3" / "Contents" / "x86-win");
  CHECK(detectBitness((root / "bits" / "x.vst3").string()) == 32);
  fs::create_directories(root / "bits" / "y.vst3" / "Contents" / "x86_64-win");
  CHECK(detectBitness((root / "bits" / "y.vst3").string()) == 64);
  CHECK(detectBitness((root / "bits" / "missing").string()) == 0);

  // --- scan + classification ---
  const fs::path lib = root / "lib";
  writePE(lib / "VSTPlugins" / "Old Synth.dll", 0x014C);       // 32-bit VST2
  writePE(lib / "VSTPlugins" / "New Synth.dll", 0x8664);       // 64-bit VST2
  writePE(lib / "Common Files" / "VST3" / "Studio EQ.vst3", 0x8664);
  { std::ofstream f(lib / "Avid" / "Studio EQ.aaxplugin"); f.put('x'); }  // dir created below
  fs::create_directories(lib / "Avid");
  { std::ofstream f(lib / "Avid" / "Studio EQ.aaxplugin"); f.put('x'); }
  { std::ofstream f(lib / "Avid" / "Only PT.aaxplugin"); f.put('x'); }
  fs::create_directories(lib / "Digidesign");
  { std::ofstream f(lib / "Digidesign" / "Old Verb.dpm"); f.put('x'); }
  { std::ofstream f(lib / "Avid" / "readme.txt"); f.put('x'); }

  HostCapabilities all;
  all.vst3 = all.vst2 = all.bridge = true;
  PluginLibrary pl(all);
  pl.scan({lib.string()});
  auto find = [&](const std::string& name, Format f) -> const LibraryEntry* {
    for (auto& e : pl.entries()) if (e.plugin.name == name && e.plugin.format == f) return &e;
    return nullptr;
  };
  CHECK(pl.entries().size() == 6);  // readme.txt ignored
  auto* oldSynth = find("Old Synth", Format::VST2);
  auto* newSynth = find("New Synth", Format::VST2);
  auto* eq3 = find("Studio EQ", Format::VST3);
  auto* eqAax = find("Studio EQ", Format::AAX);
  auto* onlyPt = find("Only PT", Format::AAX);
  auto* verb = find("Old Verb", Format::RTAS);
  CHECK(oldSynth && oldSynth->hostability == Hostability::NeedsBridge && oldSynth->plugin.is32bit);
  CHECK(newSynth && newSynth->hostability == Hostability::Native);
  CHECK(eq3 && eq3->hostability == Hostability::Native);
  CHECK(eqAax && eqAax->hostability == Hostability::NotHostable && eqAax->twin && eqAax->twin->format == Format::VST3);
  CHECK(onlyPt && onlyPt->hostability == Hostability::NotHostable && !onlyPt->twin);
  CHECK(verb && verb->hostability == Hostability::NotHostable && !verb->plugin.note.empty());

  HostCapabilities noBridge = all;
  noBridge.bridge = false;
  PluginLibrary pl2(noBridge);
  pl2.scan({lib.string()});
  for (auto& e : pl2.entries()) if (e.plugin.name == "Old Synth") CHECK(e.hostability == Hostability::NotHostable);
  PluginLibrary pl3{HostCapabilities{}};  // nothing built
  pl3.scan({lib.string()});
  for (auto& e : pl3.entries()) if (e.plugin.name == "New Synth") CHECK(e.hostability == Hostability::NotBuilt);

  // --- install ---
  const std::string home = (root / "home").string();
  auto r1 = pl.install((lib / "Common Files" / "VST3" / "Studio EQ.vst3").string(), false, home);
  CHECK(r1.ok && fs::exists(r1.installedPath));
  auto r2 = pl.install((lib / "Common Files" / "VST3" / "Studio EQ.vst3").string(), false, home);
  CHECK(!r2.ok && r2.message.find("already installed") != std::string::npos);
  auto r3 = pl.install((lib / "Common Files" / "VST3" / "Studio EQ.vst3").string(), true, home);
  CHECK(r3.ok);
  auto r4 = pl.install((lib / "Avid" / "Studio EQ.aaxplugin").string(), false, home);
  CHECK(!r4.ok && r4.message.find("Pro Tools") != std::string::npos);
  { std::ofstream f(root / "setup.exe"); f.put('x'); }
  auto r5 = pl.install((root / "setup.exe").string(), false, home);
  CHECK(!r5.ok && r5.message.find("installer") != std::string::npos);
  fs::create_directories(root / "Bundle.vst3" / "Contents" / "x86_64-win");  // bundle copied recursively
  { std::ofstream f(root / "Bundle.vst3" / "Contents" / "x86_64-win" / "Bundle.vst3"); f.put('x'); }
  auto r6 = pl.install((root / "Bundle.vst3").string(), false, home);
  CHECK(r6.ok && fs::exists(fs::path(r6.installedPath) / "Contents" / "x86_64-win" / "Bundle.vst3"));
  CHECK(!pl.install((root / "nope.vst3").string(), false, home).ok);

  // Regressions found in review:
  // (a) Installing the file that already IS the installed copy must never delete it.
  const fs::path installedEq = fs::path(r1.installedPath);
  auto r7 = pl.install(installedEq.string(), true, home);
  CHECK(r7.ok && fs::exists(installedEq));
  // (b) A trailing separator (tab-completion of a bundle) works.
  fs::create_directories(root / "Slash.vst3" / "Contents" / "x86_64-win");
  { std::ofstream f(root / "Slash.vst3" / "Contents" / "x86_64-win" / "Slash.vst3"); f.put('x'); }
  auto r8 = pl.install((root / "Slash.vst3").string() + "/", false, home);
  CHECK(r8.ok && fs::exists(fs::path(r8.installedPath) / "Contents" / "x86_64-win" / "Slash.vst3"));
  // (c) No staging or backup folders are left behind.
  for (auto& e : fs::directory_iterator(fs::path(r6.installedPath).parent_path()))
    CHECK(e.path().extension() != ".wezt-new" && e.path().extension() != ".wezt-old");

  fs::remove_all(root);
}

static void testSampleConvert() {
  using namespace audio;
  const float in[2][4] = {{0.f, 0.5f, -0.5f, 1.f}, {1.f, -1.f, 0.25f, 0.f}};
  const float* planar[2] = {in[0], in[1]};
  for (SampleFormat fmt : {SampleFormat{SampleKind::Int16, 16}, SampleFormat{SampleKind::Int24, 24},
                           SampleFormat{SampleKind::Int32, 24}, SampleFormat{SampleKind::Int32, 32},
                           SampleFormat{SampleKind::Float32, 32}}) {
    unsigned char bytes[2 * 4 * 4] = {};
    planarToInterleaved(planar, 2, 4, bytes, fmt);
    float back[2][4] = {};
    float* out[2] = {back[0], back[1]};
    interleavedToPlanar(bytes, 2, 4, out, fmt);
    const float tol = fmt.kind == SampleKind::Int16 ? 1e-3f : 1e-5f;
    for (int c = 0; c < 2; ++c)
      for (int f = 0; f < 4; ++f) CHECK(std::fabs(back[c][f] - in[c][f]) < tol);
  }
  // Out-of-range input must clip, never wrap.
  const float hot[1][2] = {{3.f, -3.f}};
  const float* hp[1] = {hot[0]};
  unsigned char b16[4];
  planarToInterleaved(hp, 1, 2, b16, SampleFormat{SampleKind::Int16, 16});
  std::int16_t s16[2];
  std::memcpy(s16, b16, 4);
  CHECK(s16[0] == 32767 && s16[1] == -32767);
}

int main() {
  testGraph();
  testTransportAndEngine();
  testRecording();
  testAaxTwins();
  testPluginLibrary();
  testSampleConvert();
  std::printf(failures ? "FAILED\n" : "all tests passed\n");
  return failures;
}
