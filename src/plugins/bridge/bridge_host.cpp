#include "plugins/bridge/bridge_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "plugins/bridge/bridge_ipc.h"
#include "plugins/bridge/bridge_protocol.h"

namespace wezt::plugins {
using namespace bridge;

struct BridgedPluginInstance::Impl {
  BridgeConfig config;
  std::string pluginPath;
  unsigned channels;
  IpcHost ipc;
  std::mutex setMutex;                           // serializes control threads (UI, AI agent) that set or read values
  std::unordered_map<unsigned, double> lastSet;  // guarded by setMutex
  Header* header = nullptr;
  float* in = nullptr;
  float* out = nullptr;
  std::uint8_t* blob = nullptr;
  std::uint32_t maxFrames = 0;
  std::atomic<bool> ready{false}, crashed{false};
  std::atomic<unsigned> dropped{0};
  std::string error;

  // One request is in flight at most. After a timeout the helper may still be working on it and
  // will ack later; that late ack must be consumed before anything new is sent, otherwise it would
  // be mistaken for the answer to the next request and the host would read buffers the helper is
  // still writing.
  bool outstanding = false;
  std::chrono::steady_clock::time_point sentAt{};
  static constexpr int kHungAfterMs = 2000;  // alive but silent this long = treated as crashed

  bool command(Command c, int timeoutMs) {
    if (outstanding) {
      if (!ipc.waitHelper(0)) return false;  // still busy: skip, never block again on the same stall
      outstanding = false;
    }
    header->command.store(c, std::memory_order_release);
    if (!ipc.signalHelper()) return false;
    outstanding = true;
    sentAt = std::chrono::steady_clock::now();
    if (!ipc.waitHelper(timeoutMs)) return false;
    outstanding = false;
    return true;
  }
  bool hungTooLong() const {
    return outstanding && std::chrono::steady_clock::now() - sentAt > std::chrono::milliseconds(kHungAfterMs);
  }
  void forgetMapping() { header = nullptr; in = out = nullptr; blob = nullptr; }
};

BridgedPluginInstance::BridgedPluginInstance(BridgeConfig config, std::string pluginPath, unsigned channels)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->pluginPath = std::move(pluginPath);
  impl_->channels = channels;
}

BridgedPluginInstance::~BridgedPluginInstance() {
  auto& d = *impl_;
  if (d.header && d.ready.load() && !d.crashed.load()) {
    d.header->command.store(kCmdQuit, std::memory_order_release);
    d.ipc.signalHelper();
  }
  d.ipc.shutdown();
}

void BridgedPluginInstance::prepare(double sampleRate, std::size_t maxFrames) {
  auto& d = *impl_;
  if (d.header && d.ready.load() && !d.crashed.load()) {  // let the old helper close the plugin properly
    d.header->command.store(kCmdQuit, std::memory_order_release);
    d.ipc.signalHelper();
  }
  d.ipc.shutdown();
  d.forgetMapping();
  d.ready = false;
  d.crashed = false;
  d.outstanding = false;
  d.dropped = 0;
  d.maxFrames = static_cast<std::uint32_t>(maxFrames);

  const Layout lay = Layout::compute(d.channels, d.maxFrames);
  if (!d.ipc.create(lay.total, &d.error)) return;
  auto* base = static_cast<std::uint8_t*>(d.ipc.data());
  d.header = reinterpret_cast<Header*>(base);
  d.in = reinterpret_cast<float*>(base + lay.inOffset);
  d.out = reinterpret_cast<float*>(base + lay.outOffset);
  d.blob = base + lay.blobOffset;
  d.header->magic = kMagic;
  d.header->version = kVersion;
  d.header->channels = d.channels;
  d.header->maxFrames = d.maxFrames;
  d.header->sampleRate = sampleRate;
  d.header->helperState.store(kHelperStarting);

  std::vector<std::string> extra = {"--plugin", d.pluginPath};
  extra.insert(extra.end(), d.config.helperArgs.begin(), d.config.helperArgs.end());
  if (!d.ipc.spawn(d.config.helperExecutable, extra, &d.error)) return;
  // The helper signals once when it is ready (or has failed).
  if (!d.ipc.waitHelper(d.config.startupTimeoutMs)) {
    d.error = "helper did not start in time or exited";
    d.crashed = !d.ipc.helperAlive();
    return;
  }
  if (d.header->helperState.load() != kHelperReady) {
    d.error = d.header->error;
    return;
  }
  d.ipc.unlinkShm();  // helper is attached: the name is no longer needed, and must not outlive a killed host
  d.ready = true;
}

void BridgedPluginInstance::process(const AudioBuffer& in, AudioBuffer& out) {
  auto& d = *impl_;
  const std::uint32_t frames = static_cast<std::uint32_t>(out.frames);
  if (!d.ready.load(std::memory_order_acquire) || d.crashed.load(std::memory_order_relaxed) || frames == 0 ||
      frames > d.maxFrames) {
    out.clear();
    return;
  }
  for (unsigned c = 0; c < d.channels; ++c) {
    float* dst = d.in + static_cast<std::size_t>(c) * d.maxFrames;
    if (c < in.channels && in.frames >= frames) std::copy(in.channel(c), in.channel(c) + frames, dst);
    else std::fill(dst, dst + frames, 0.f);
  }
  d.header->frames.store(frames, std::memory_order_relaxed);
  if (!d.command(kCmdProcess, d.config.timeoutMs)) {
    d.dropped.fetch_add(1);
    out.clear();
    if (!d.ipc.helperAlive() || d.hungTooLong()) d.crashed = true;
    return;
  }
  for (std::size_t c = 0; c < out.channels; ++c) {
    if (c < d.channels) std::copy(d.out + c * d.maxFrames, d.out + c * d.maxFrames + frames, out.channel(c));
    else std::fill(out.channel(c), out.channel(c) + frames, 0.f);
  }
}

std::size_t BridgedPluginInstance::latencySamples() const {
  return impl_->header && impl_->ready.load() && !impl_->crashed.load() ? static_cast<std::size_t>(std::max(0, impl_->header->latency.load())) : 0;
}

std::size_t BridgedPluginInstance::parameterCount() const {
  return impl_->header && impl_->ready.load() && !impl_->crashed.load() ? impl_->header->paramCount.load() : 0;
}

std::vector<ParamInfo> BridgedPluginInstance::parameters() const {
  auto& d = *impl_;
  std::vector<ParamInfo> out;
  if (!d.ready.load() || d.crashed.load()) return out;
  if (!d.command(kCmdListParams, 5000) || d.header->result.load() != 1) return out;
  const auto n = d.header->blobSize.load() / sizeof(ParamListEntry);
  const auto* e = reinterpret_cast<const ParamListEntry*>(d.blob);
  for (std::size_t i = 0; i < n; ++i) {
    ParamInfo p;
    p.id = e[i].id;
    p.name = std::string(e[i].name, strnlen(e[i].name, sizeof e[i].name));
    p.defaultNormalized = e[i].defaultNormalized;
    p.hidden = e[i].hidden != 0;
    out.push_back(std::move(p));
  }
  return out;
}

bool BridgedPluginInstance::setParameterNormalized(unsigned id, double value) {
  auto& d = *impl_;
  if (!d.header || !d.ready.load() || d.crashed.load()) return false;
  std::lock_guard<std::mutex> lock(d.setMutex);
  const auto h = d.header->paramHead.load(std::memory_order_relaxed);
  const auto n = (h + 1) % kParamRingSize;
  if (n == d.header->paramTail.load(std::memory_order_acquire)) return false;
  d.header->params[h] = {id, std::clamp(value, 0.0, 1.0)};
  d.header->paramHead.store(n, std::memory_order_release);
  d.lastSet[id] = std::clamp(value, 0.0, 1.0);
  return true;
}

bool BridgedPluginInstance::automateParameter(unsigned id, double value) {   // audio thread: shared-memory ring, never waits
  auto& d = *impl_;
  if (!d.header || !d.ready.load() || d.crashed.load()) return false;
  std::unique_lock<std::mutex> lock(d.setMutex, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  const auto h = d.header->paramHead.load(std::memory_order_relaxed);
  const auto n = (h + 1) % kParamRingSize;
  if (n == d.header->paramTail.load(std::memory_order_acquire)) return false;
  d.header->params[h] = {id, std::clamp(value, 0.0, 1.0)};
  d.header->paramHead.store(n, std::memory_order_release);
  return true;   // lastSet stays as the control thread last set it (the map cannot be touched here without allocating)
}

double BridgedPluginInstance::parameterNormalized(unsigned id) const {
  std::lock_guard<std::mutex> lock(impl_->setMutex);
  const auto& m = impl_->lastSet;
  const auto it = m.find(id);
  return it != m.end() ? it->second : 0.0;
}

std::vector<std::uint8_t> BridgedPluginInstance::saveState() const {
  auto& d = *impl_;
  if (!d.ready.load() || d.crashed.load()) return {};
  if (!d.command(kCmdSaveState, 10000) || d.header->result.load() != 1) return {};
  const auto n = d.header->blobSize.load();
  return std::vector<std::uint8_t>(d.blob, d.blob + n);
}

bool BridgedPluginInstance::loadState(const std::vector<std::uint8_t>& state) {
  auto& d = *impl_;
  if (!d.ready.load() || d.crashed.load() || state.empty() || state.size() > kStateCapacity) return false;
  std::memcpy(d.blob, state.data(), state.size());
  d.header->blobSize.store(static_cast<std::uint32_t>(state.size()));
  return d.command(kCmdLoadState, 10000) && d.header->result.load() == 1;
}

bool BridgedPluginInstance::ready() const { return impl_->ready.load(); }
bool BridgedPluginInstance::crashed() const { return impl_->crashed.load(); }
unsigned BridgedPluginInstance::droppedBlocks() const { return impl_->dropped.load(); }
std::string BridgedPluginInstance::pluginName() const {
  return impl_->header && impl_->ready.load() && !impl_->crashed.load() ? std::string(impl_->header->pluginName) : std::string();
}
std::string BridgedPluginInstance::lastError() const { return impl_->error; }

void BridgedPluginInstance::killHelperProcessForTesting() { impl_->ipc.killChild(); }
long BridgedPluginInstance::helperPidForTesting() const { return impl_->ipc.childPid(); }

void BridgedPluginInstance::killHelperForTesting() {
  // Shutdown path kills a helper that does not exit on its own; used by tests to simulate a crash.
  impl_->crashed = true;  // first: every accessor checks it before touching the mapping
  impl_->ipc.shutdown();
  impl_->forgetMapping();
}

}  // namespace wezt::plugins
