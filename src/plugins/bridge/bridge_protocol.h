#pragma once
// Shared-memory protocol between Wezt (host) and wezt-bridge (helper process).
//
// Why a separate process: a 32-bit plugin cannot be loaded into a 64-bit process. The helper is
// built with the plugin's bitness and hosts it; audio, parameters and state cross in shared
// memory. A plugin crash kills only the helper, never the session.
//
// The bridge is transparent. It passes 32-bit float audio through unchanged and adds no
// processing of its own, so a bridged plugin sounds exactly like the same plugin run natively.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wezt::plugins::bridge {

constexpr std::uint32_t kMagic = 0x525A5757;  // "WWZR"
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kStateCapacity = 16u * 1024u * 1024u;  // state / param-list blob
constexpr std::size_t kParamRingSize = 256;

enum Command : std::uint32_t {
  kCmdProcess = 0,
  kCmdSaveState = 1,
  kCmdLoadState = 2,
  kCmdListParams = 3,
  kCmdQuit = 4,
};

enum HelperState : std::uint32_t { kHelperStarting = 0, kHelperReady = 1, kHelperFailed = 2 };

struct ParamEdit {
  std::uint32_t id;
  double value;
};

struct ParamListEntry {  // written by the helper for kCmdListParams
  std::uint32_t id;
  std::uint32_t hidden;
  double defaultNormalized;
  char name[64];
};

struct Header {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t channels;
  std::uint32_t maxFrames;
  double sampleRate;

  std::atomic<std::uint32_t> helperState;
  std::atomic<std::uint32_t> command;
  std::atomic<std::uint32_t> frames;       // frames in the current kCmdProcess request
  std::atomic<std::int32_t> latency;       // samples, written by helper after prepare
  std::atomic<std::uint32_t> paramCount;   // written by helper after load
  std::atomic<std::uint32_t> blobSize;     // bytes used in the state blob (both directions)
  std::atomic<std::uint32_t> result;       // 1 = ok, 0 = failed (for the last command)

  std::atomic<std::uint32_t> paramHead, paramTail;
  ParamEdit params[kParamRingSize];

  char pluginName[128];
  char error[256];
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "needs lock-free atomics across processes");

constexpr std::size_t alignUp(std::size_t n, std::size_t a) { return (n + a - 1) / a * a; }

struct Layout {
  std::size_t inOffset, outOffset, blobOffset, total;
  static Layout compute(std::uint32_t channels, std::uint32_t maxFrames) {
    Layout l;
    const std::size_t audioBytes = static_cast<std::size_t>(channels) * maxFrames * sizeof(float);
    l.inOffset = alignUp(sizeof(Header), 64);
    l.outOffset = l.inOffset + alignUp(audioBytes, 64);
    l.blobOffset = l.outOffset + alignUp(audioBytes, 64);
    l.total = l.blobOffset + kStateCapacity;
    return l;
  }
};

}  // namespace wezt::plugins::bridge
