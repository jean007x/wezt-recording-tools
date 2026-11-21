// Runs a real VST3 plugin through the bridge (a separate wezt-bridge process, audio over shared
// memory) and checks that the result is identical to running the same plugin in-process.
//   usage: wezt_bridge_test <wezt-bridge executable> <plugin.vst3>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <fstream>
#endif

#include "engine/graph.h"
#include "plugins/bridge/bridge_host.h"
#include "plugins/bridge/bridge_ipc.h"
#include "plugins/vst3_host.h"

using namespace wezt;
using namespace wezt::plugins;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static AudioBuffer sineBlock(unsigned frames, double phase) {
  AudioBuffer b(2, frames);
  for (unsigned i = 0; i < frames; ++i) {
    const float v = static_cast<float>(0.8 * std::sin(phase + 2.0 * 3.14159265358979323846 * 1000.0 * i / 48000.0));
    b.channel(0)[i] = v;
    b.channel(1)[i] = v * 0.5f;
  }
  return b;
}


static float peakOfBuf(const AudioBuffer& b) {
  float p = 0;
  for (float v : b.data) p = std::fmax(p, std::fabs(v));
  return p;
}
static AudioBuffer constBlock(unsigned frames, float v) {
  AudioBuffer b(2, frames);
  std::fill(b.data.begin(), b.data.end(), v);
  return b;
}
using clk = std::chrono::steady_clock;
static double msSince(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

#if !defined(_WIN32)
// A process that is gone, or dead but not yet reaped, is not running any more.
static bool processGone(long pid) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
  if (!f) return true;
  std::string line;
  std::getline(f, line);
  const auto rp = line.rfind(')');
  return rp == std::string::npos || line.size() < rp + 3 || line[rp + 2] == 'Z';
}
static int countShmSegments(long hostPid) {
  int n = 0;
  if (DIR* d = opendir("/dev/shm")) {
    const std::string prefix = "wezt-" + std::to_string(hostPid) + "-";
    while (dirent* e = readdir(d)) if (std::string(e->d_name).rfind(prefix, 0) == 0) ++n;
    closedir(d);
  }
  return n;
}
#endif

static void testQuoting() {
  using bridge::quoteWindowsArg;
  CHECK(quoteWindowsArg("C:\\Program Files\\x.vst3") == "\"C:\\Program Files\\x.vst3\"");
  CHECK(quoteWindowsArg("C:\\dir\\") == "\"C:\\dir\\\\\"");        // trailing backslash is doubled
  CHECK(quoteWindowsArg("say \"hi\"") == "\"say \\\"hi\\\"\"");     // quotes are escaped
  CHECK(quoteWindowsArg("a\\\"b") == "\"a\\\\\\\"b\"");           // backslash before a quote: doubled, then the quote escaped
  CHECK(quoteWindowsArg("") == "\"\"");
}

int main(int argc, char** argv) {
  testQuoting();
  if (argc < 3) {
    std::printf("usage: wezt_bridge_test <wezt-bridge> <plugin.vst3>  (skipped)\n");
    return 0;
  }
  const std::string helper = argv[1], pluginPath = argv[2];
  std::string err;

  // An instrument in safe mode: notes cross the bridge and make sound; an effect reports no MIDI.
  if (argc >= 4) {
    BridgeConfig cfg;
    cfg.helperExecutable = helper;
    cfg.helperArgs = {"--class", argc >= 5 ? argv[4] : "mda Piano"};
    BridgedPluginInstance b(cfg, argv[3]);
    b.prepare(48000, 256);
    CHECK(b.ready());
    CHECK(b.acceptsMidi());
    AudioBuffer in(2, 256), out(2, 256);
    float before = 0, after = 0;
    for (int i = 0; i < 4; ++i) { b.process(in, out); before = std::fmax(before, peakOfBuf(out)); }
    MidiEvent on;
    on.type = MidiEvent::NoteOn; on.a = 69; on.b = 110; on.offset = 0;
    b.handleMidi(&on, 1);
    b.process(in, out);
    after = peakOfBuf(out);
    for (int i = 0; i < 30; ++i) { b.process(in, out); after = std::fmax(after, peakOfBuf(out)); }
    std::printf("bridged instrument: peak before note %.6f, after %.4f\n", before, after);
    CHECK(before < 1e-4f);
    CHECK(after > 0.01f);
  }
  {
    BridgeConfig cfg;
    cfg.helperExecutable = helper;
    BridgedPluginInstance e(cfg, pluginPath);
    e.prepare(48000, 256);
    if (e.ready()) CHECK(!e.acceptsMidi());
  }

  // A helper that cannot start: clear failure, no hang, silence out.
  {
    BridgeConfig bad;
    bad.helperExecutable = "/definitely/not/a/helper";
    BridgedPluginInstance b(bad, pluginPath);
    b.prepare(48000, 256);
    CHECK(!b.ready() && !b.lastError().empty());
    AudioBuffer in = sineBlock(256, 0), out(2, 256);
    b.process(in, out);
    float p = 0;
    for (float v : out.data) p = std::fmax(p, std::fabs(v));
    CHECK(p == 0.f);
  }
  // A plugin that does not exist: the helper reports why.
  {
    BridgeConfig cfg;
    cfg.helperExecutable = helper;
    BridgedPluginInstance b(cfg, "/no/such/plugin.vst3");
    b.prepare(48000, 256);
    CHECK(!b.ready());
    CHECK(b.lastError().find("cannot load") != std::string::npos);
  }

  // Reference: the plugin in-process.
  auto direct = Vst3Instance::load(pluginPath, &err);
  if (!direct) { std::printf("load failed: %s\n", err.c_str()); return 1; }
  direct->prepare(48000, 256);
  const auto params = direct->parameters();
  unsigned gainId = 0;
  double gainDefault = 1.0;
  bool found = false;
  for (auto& p : params)
    if (p.name.find("Gain") != std::string::npos) { gainId = p.id; gainDefault = p.defaultNormalized; found = true; }
  CHECK(found);

  // Same plugin through the bridge.
  BridgeConfig cfg;
  cfg.helperExecutable = helper;
  BridgedPluginInstance bridged(cfg, pluginPath);
  bridged.prepare(48000, 256);
  CHECK(bridged.ready());
  if (!bridged.ready()) { std::printf("bridge not ready: %s\n", bridged.lastError().c_str()); return 1; }
  CHECK(bridged.pluginName() == direct->name());
  CHECK(bridged.parameterCount() == direct->parameterCount());
  auto bparams = bridged.parameters();
  CHECK(bparams.size() == params.size());
  CHECK(bridged.latencySamples() == direct->latencySamples());

  // Bit-exact comparison across several blocks and parameter changes.
  const double gains[] = {gainDefault, 0.5, 0.0, 0.25};
  double maxDiff = 0;
  for (int step = 0; step < 4; ++step) {
    direct->setParameterNormalized(gainId, gains[step]);
    bridged.setParameterNormalized(gainId, gains[step]);
    for (int blk = 0; blk < 3; ++blk) {
      AudioBuffer in = sineBlock(256, step * 1.0 + blk * 0.1);
      AudioBuffer a(2, 256), b(2, 256);
      direct->process(in, a);
      bridged.process(in, b);
      for (std::size_t i = 0; i < a.data.size(); ++i) maxDiff = std::fmax(maxDiff, std::fabs(a.data[i] - b.data[i]));
    }
  }
  std::printf("max difference bridged vs in-process: %g\n", maxDiff);
  CHECK(maxDiff == 0.0);  // the bridge is transparent
  CHECK(bridged.droppedBlocks() == 0 && !bridged.crashed());

  // State round trip across the process boundary.
  auto state = bridged.saveState();
  CHECK(!state.empty());
  CHECK(bridged.loadState(state));


#if !defined(_WIN32)
  // --- 1. The helper dies while idle. Writing to its dead pipe must not raise a fatal SIGPIPE. ---
  {
    BridgedPluginInstance b(cfg, pluginPath);
    b.prepare(48000, 256);
    CHECK(b.ready());
    b.killHelperProcessForTesting();  // like a plugin segfault: the pipes stay open on our side
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    AudioBuffer in = constBlock(256, 0.3f), out(2, 256);
    for (int i = 0; i < 3; ++i) b.process(in, out);  // dies here if SIGPIPE is not handled
    CHECK(peakOfBuf(out) == 0.f);
    CHECK(b.crashed());
  }

  // --- 2. A slow block must not desynchronise the protocol, and must not stall the audio thread. ---
  {
    BridgeConfig slow = cfg;
    slow.timeoutMs = 50;
    slow.helperArgs = {"--test-stall-ms", "200", "--test-stall-at", "3"};  // block 3 takes 200 ms
    BridgedPluginInstance b(slow, pluginPath);
    b.prepare(48000, 256);
    CHECK(b.ready());
    b.setParameterNormalized(gainId, 1.0);  // gain 1: output must equal input exactly
    int ok = 0, silent = 0, wrong = 0, slowCalls = 0, afterStallOk = 0;
    bool stalled = false;
    for (int blk = 1; blk <= 40; ++blk) {
      const float v = 0.1f + 0.01f * static_cast<float>(blk);
      AudioBuffer in = constBlock(256, v), out(2, 256);
      const auto t0 = clk::now();
      b.process(in, out);
      if (msSince(t0) > 30.0) ++slowCalls;
      if (peakOfBuf(out) == 0.f) { ++silent; if (blk >= 3) stalled = true; }
      else {
        // An answer to an older block (the desync symptom) would carry a different constant.
        bool exact = true;
        for (float x : out.data) if (std::fabs(x - v) > 1e-6f) exact = false;
        exact ? ++ok : ++wrong;
        if (stalled) ++afterStallOk;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("slow block: %d ok, %d dropped, %d wrong, %d slow calls\n", ok, silent, wrong, slowCalls);
    CHECK(wrong == 0);                    // never an answer that belongs to a different block
    CHECK(silent >= 1);                   // the stalled block was dropped, and said so
    CHECK(b.droppedBlocks() >= 1);
    CHECK(slowCalls <= 2);                // only the first timed-out call may wait, not every block of the stall
    CHECK(afterStallOk >= 5);             // and it recovers
    CHECK(!b.crashed());                  // 200 ms is a hiccup, not a crash
  }

  // --- 3. A helper that hangs for good is declared crashed, and never blocks the audio thread block after block. ---
  {
    BridgeConfig hung = cfg;
    hung.timeoutMs = 50;
    hung.helperArgs = {"--test-stall-ms", "30000", "--test-stall-at", "2"};
    BridgedPluginInstance b(hung, pluginPath);
    b.prepare(48000, 256);
    CHECK(b.ready());
    int slowCalls = 0;
    const auto start = clk::now();
    while (!b.crashed() && msSince(start) < 6000) {
      AudioBuffer in = constBlock(256, 0.2f), out(2, 256);
      const auto t0 = clk::now();
      b.process(in, out);
      if (msSince(t0) > 30.0) ++slowCalls;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("hung helper: crashed=%d after %.0f ms, %d slow calls\n", b.crashed() ? 1 : 0, msSince(start), slowCalls);
    CHECK(b.crashed());
    CHECK(slowCalls <= 2);
  }

  // --- 4. The host dies abruptly: its helper must notice and exit, and leave no shared memory behind. ---
  {
    int fds[2];
    CHECK(pipe(fds) == 0);
    const pid_t host = fork();
    if (host == 0) {
      close(fds[0]);
      BridgedPluginInstance b(cfg, pluginPath);
      b.prepare(48000, 256);
      const long helperPid = b.helperPidForTesting();
      const long me = getpid();
      ssize_t w1 = write(fds[1], &helperPid, sizeof helperPid), w2 = write(fds[1], &me, sizeof me);
      (void)w1; (void)w2;
      _exit(b.ready() ? 0 : 1);  // no destructors: this is what SIGKILL / a crash looks like to the helper
    }
    close(fds[1]);
    long helperPid = 0, hostPid = 0;
    CHECK(read(fds[0], &helperPid, sizeof helperPid) == static_cast<ssize_t>(sizeof helperPid));
    CHECK(read(fds[0], &hostPid, sizeof hostPid) == static_cast<ssize_t>(sizeof hostPid));
    close(fds[0]);
    int status = 0;
    waitpid(host, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(helperPid > 0);
    bool gone = false;
    for (int i = 0; i < 60 && !gone; ++i) {  // the helper polls every 500 ms; give it a few seconds
      gone = processGone(helperPid);
      if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(gone);
    CHECK(countShmSegments(hostPid) == 0);
    if (!gone) kill(static_cast<pid_t>(helperPid), SIGKILL);
  }
#endif

  // A plugin host crash must not take the session down: kill the helper, keep processing.
  bridged.killHelperForTesting();
  AudioBuffer in = sineBlock(256, 0), out(2, 256);
  bridged.process(in, out);  // must return promptly with silence
  float p = 0;
  for (float v : out.data) p = std::fmax(p, std::fabs(v));
  CHECK(p == 0.f && bridged.crashed());

  std::printf(failures ? "FAILED\n" : "bridge tests passed\n");
  return failures;
}
