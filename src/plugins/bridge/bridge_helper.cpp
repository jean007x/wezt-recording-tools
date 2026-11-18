// wezt-bridge: hosts one plugin in its own process and serves audio requests from Wezt over shared
// memory (see bridge_protocol.h). Build it with the same bitness as the plugin; for 32-bit plugins
// on Windows that means a 32-bit build (cmake -A Win32), which the 64-bit Wezt then launches.
//
// The helper adds no processing of its own: audio goes in as 32-bit float and comes back exactly as
// the plugin produced it.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <memory>
#include <thread>

#include "plugins/bridge/bridge_ipc.h"
#include "plugins/bridge/bridge_protocol.h"
#include "plugins/vst3_host.h"

using namespace wezt;
using namespace wezt::plugins;
using namespace wezt::plugins::bridge;

namespace {

void copyStr(char* dst, std::size_t cap, const std::string& s) {
  std::strncpy(dst, s.c_str(), cap - 1);
  dst[cap - 1] = '\0';
}

}  // namespace

int main(int argc, char** argv) {
  const ArgMap args = parseArgs(argc, argv);
  IpcHelper ipc;
  std::string err;
  if (!ipc.attach(args, &err)) {
    std::fprintf(stderr, "wezt-bridge: %s\n", err.c_str());
    return 2;
  }
  auto* h = static_cast<Header*>(ipc.data());
  auto fail = [&](const std::string& why) {
    copyStr(h->error, sizeof h->error, why);
    h->helperState.store(kHelperFailed, std::memory_order_release);
    ipc.signalDone();
    return 3;
  };
  if (h->magic != kMagic || h->version != kVersion) return fail("protocol mismatch");

  const auto pluginIt = args.find("plugin");
  if (pluginIt == args.end()) return fail("no --plugin given");
  auto inst = Vst3Instance::load(pluginIt->second, &err);
  if (!inst) return fail(err);

  const Layout lay = Layout::compute(h->channels, h->maxFrames);
  auto* base = static_cast<std::uint8_t*>(ipc.data());
  float* inAudio = reinterpret_cast<float*>(base + lay.inOffset);
  float* outAudio = reinterpret_cast<float*>(base + lay.outOffset);
  std::uint8_t* blob = base + lay.blobOffset;
  const std::uint32_t channels = h->channels, maxFrames = h->maxFrames;

  inst->prepare(h->sampleRate, maxFrames);
  copyStr(h->pluginName, sizeof h->pluginName, inst->name());
  h->paramCount.store(static_cast<std::uint32_t>(inst->parameterCount()));
  h->latency.store(static_cast<std::int32_t>(inst->latencySamples()));
  h->helperState.store(kHelperReady, std::memory_order_release);
  ipc.signalDone();  // "ready"

  AudioBuffer inBuf(channels, maxFrames), outBuf(channels, maxFrames);

  // Diagnostics for tests: pretend the plugin hangs for --test-stall-ms on the Nth process block.
  auto argInt = [&](const char* k) { auto it = args.find(k); return it == args.end() ? 0L : std::strtol(it->second.c_str(), nullptr, 10); };
  const long stallMs = argInt("test-stall-ms"), stallAt = argInt("test-stall-at");
  long blockCount = 0;

  for (;;) {
    bool hostGone = false;
    if (!ipc.waitRequest(500, &hostGone)) {
      if (hostGone) break;
      continue;
    }
    switch (static_cast<Command>(h->command.load(std::memory_order_acquire))) {
      case kCmdProcess: {
        // Apply queued parameter edits, then run the block.
        for (;;) {
          const auto t = h->paramTail.load(std::memory_order_relaxed);
          if (t == h->paramHead.load(std::memory_order_acquire)) break;
          const ParamEdit e = h->params[t];
          h->paramTail.store((t + 1) % kParamRingSize, std::memory_order_release);
          inst->setParameterNormalized(e.id, e.value);
        }
        const std::uint32_t frames = std::min(h->frames.load(std::memory_order_relaxed), maxFrames);
        inBuf.setFrames(frames);  // buffers keep their maxFrames capacity: no reallocation per block
        outBuf.setFrames(frames);
        for (std::uint32_t c = 0; c < channels; ++c)
          std::copy(inAudio + static_cast<std::size_t>(c) * maxFrames, inAudio + static_cast<std::size_t>(c) * maxFrames + frames,
                    inBuf.channel(c));
        if (stallMs > 0 && ++blockCount == stallAt) std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
        inst->process(inBuf, outBuf);
        for (std::uint32_t c = 0; c < channels; ++c)
          std::copy(outBuf.channel(c), outBuf.channel(c) + frames, outAudio + static_cast<std::size_t>(c) * maxFrames);
        h->latency.store(static_cast<std::int32_t>(inst->latencySamples()), std::memory_order_relaxed);
        break;
      }
      case kCmdSaveState: {
        const auto s = inst->saveState();
        const bool ok = !s.empty() && s.size() <= kStateCapacity;
        if (ok) std::memcpy(blob, s.data(), s.size());
        h->blobSize.store(ok ? static_cast<std::uint32_t>(s.size()) : 0);
        h->result.store(ok ? 1 : 0);
        break;
      }
      case kCmdLoadState: {
        const auto n = h->blobSize.load();
        std::vector<std::uint8_t> s(blob, blob + std::min<std::size_t>(n, kStateCapacity));
        h->result.store(inst->loadState(s) ? 1 : 0);
        break;
      }
      case kCmdListParams: {
        auto* entries = reinterpret_cast<ParamListEntry*>(blob);
        const auto params = inst->parameters();
        const std::size_t n = std::min<std::size_t>(params.size(), kStateCapacity / sizeof(ParamListEntry));
        for (std::size_t i = 0; i < n; ++i) {
          entries[i] = {};
          entries[i].id = params[i].id;
          entries[i].hidden = params[i].hidden ? 1 : 0;
          entries[i].defaultNormalized = params[i].defaultNormalized;
          copyStr(entries[i].name, sizeof entries[i].name, params[i].name);
        }
        h->blobSize.store(static_cast<std::uint32_t>(n * sizeof(ParamListEntry)));
        h->result.store(1);
        break;
      }
      case kCmdQuit:
        ipc.signalDone();
        return 0;
    }
    ipc.signalDone();
  }
  return 0;
}
