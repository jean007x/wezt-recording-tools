#pragma once
// Flushes denormal floats to zero for the lifetime of the object. Feedback filters and reverb
// tails decay into denormals, which are 10-100x slower on many CPUs and cause sudden CPU spikes
// ("crackles") exactly when a track goes quiet. Applies to the calling thread only.
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif
#include <cstdint>

namespace wezt {

class ScopedNoDenormals {
 public:
  ScopedNoDenormals() {
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
    saved_ = _mm_getcsr();
    _mm_setcsr(saved_ | 0x8040u);  // FTZ (bit 15) | DAZ (bit 6)
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    saved_ = v;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v | (1ull << 24)));  // FZ
#endif
  }
  ~ScopedNoDenormals() {
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
    _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_));
#endif
  }
  ScopedNoDenormals(const ScopedNoDenormals&) = delete;
  ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

 private:
  std::uint64_t saved_ = 0;
};

}  // namespace wezt
