// Several control threads (the UI and an AI agent) pushing parameter edits to one plugin while the audio
// thread pops: nothing may be torn, duplicated or invented.
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "plugins/param_ring.h"

using namespace wezt::plugins;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  constexpr int kProducers = 4, kPer = 200000;
  ParamRing<512> ring;
  std::atomic<bool> done{false};
  std::atomic<long> accepted[kProducers] = {};
  std::vector<std::vector<double>> last(kProducers);
  std::vector<long> popped(kProducers, 0);
  bool torn = false, order = false;
  std::thread audio([&] {
    ParamRing<512>::Item it; std::vector<double> prev(kProducers, -1);
    auto drain = [&] { while (ring.pop(it)) {
      const int p = static_cast<int>(it.index);
      if (p < 0 || p >= kProducers) { torn = true; continue; }
      if (it.value < p * 1e7 || it.value >= p * 1e7 + kPer) torn = true;   // value encodes the producer: a torn item would not
      if (it.value <= prev[static_cast<std::size_t>(p)]) order = true;                              // each producer's edits stay in order
      prev[static_cast<std::size_t>(p)] = it.value; ++popped[static_cast<std::size_t>(p)];
    } };
    while (!done.load()) drain();
    drain();
  });
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) producers.emplace_back([&, p] {
    for (int i = 0; i < kPer; ++i) {   // a full queue drops an edit by design; here the producer waits for room so every edit must arrive, in order
      while (!ring.push({static_cast<std::uint32_t>(p), p * 1e7 + i})) std::this_thread::yield();
      accepted[p]++;
    }
  });
  for (auto& t : producers) t.join();
  done = true; audio.join();
  for (int p = 0; p < kProducers; ++p) { std::printf("producer %d: accepted %ld, applied %ld\n", p, accepted[p].load(), popped[static_cast<std::size_t>(p)]); CHECK(accepted[p].load() == popped[static_cast<std::size_t>(p)]); CHECK(accepted[p].load() == kPer); }
  CHECK(!torn); CHECK(!order);
  if (failures == 0) std::printf("parameter ring tests passed\n");
  return failures == 0 ? 0 : 1;
}
