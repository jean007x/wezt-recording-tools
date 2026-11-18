#pragma once
// Planar float <-> interleaved integer conversion, used by exclusive-mode backends where the
// device dictates the sample format (usually 24-bit). Header-only, no platform dependencies.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wezt::audio {

enum class SampleKind { Float32, Int16, Int24, Int32 };  // Int32 = 32-bit container

struct SampleFormat {
  SampleKind kind = SampleKind::Float32;
  unsigned validBits = 32;  // significant bits (24 for "24-in-32" containers)
  unsigned bytesPerSample() const {
    switch (kind) {
      case SampleKind::Int16: return 2;
      case SampleKind::Int24: return 3;
      default: return 4;
    }
  }
};

namespace detail {
inline std::int32_t quantize(float x, unsigned bits) {
  const float c = std::isfinite(x) ? std::clamp(x, -1.f, 1.f) : (x > 0.f ? 1.f : 0.f);  // NaN -> silence
  const double scale = static_cast<double>((1LL << (bits - 1)) - 1);
  return static_cast<std::int32_t>(std::lrint(static_cast<double>(c) * scale));
}
inline float dequantize(std::int32_t v, unsigned bits) {
  return static_cast<float>(static_cast<double>(v) / static_cast<double>(1LL << (bits - 1)));
}
}  // namespace detail

inline void planarToInterleaved(const float* const* src, unsigned channels, unsigned frames, void* dst, SampleFormat f) {
  auto* p8 = static_cast<std::uint8_t*>(dst);
  for (unsigned i = 0; i < frames; ++i) {
    for (unsigned c = 0; c < channels; ++c) {
      const float x = src[c][i];
      switch (f.kind) {
        case SampleKind::Float32: {
          std::memcpy(p8, &x, 4);
          break;
        }
        case SampleKind::Int16: {
          const auto v = static_cast<std::int16_t>(detail::quantize(x, 16));
          std::memcpy(p8, &v, 2);
          break;
        }
        case SampleKind::Int24: {
          const std::int32_t v = detail::quantize(x, 24);
          p8[0] = static_cast<std::uint8_t>(v & 0xFF);
          p8[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
          p8[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
          break;
        }
        case SampleKind::Int32: {
          // Left-justified: a 24-bit-valid sample occupies the top 24 bits of the container.
          const std::int32_t v = detail::quantize(x, f.validBits) << (32 - f.validBits);
          std::memcpy(p8, &v, 4);
          break;
        }
      }
      p8 += f.bytesPerSample();
    }
  }
}

inline void interleavedToPlanar(const void* src, unsigned channels, unsigned frames, float* const* dst, SampleFormat f) {
  const auto* p8 = static_cast<const std::uint8_t*>(src);
  for (unsigned i = 0; i < frames; ++i) {
    for (unsigned c = 0; c < channels; ++c) {
      float x = 0.f;
      switch (f.kind) {
        case SampleKind::Float32: std::memcpy(&x, p8, 4); break;
        case SampleKind::Int16: {
          std::int16_t v;
          std::memcpy(&v, p8, 2);
          x = detail::dequantize(v, 16);
          break;
        }
        case SampleKind::Int24: {
          std::int32_t v = static_cast<std::int32_t>(p8[0]) | (static_cast<std::int32_t>(p8[1]) << 8) |
                           (static_cast<std::int32_t>(p8[2]) << 16);
          if (v & 0x800000) v |= ~0xFFFFFF;  // sign-extend
          x = detail::dequantize(v, 24);
          break;
        }
        case SampleKind::Int32: {
          std::int32_t v;
          std::memcpy(&v, p8, 4);
          x = detail::dequantize(v >> (32 - f.validBits), f.validBits);
          break;
        }
      }
      dst[c][i] = x;
      p8 += f.bytesPerSample();
    }
  }
}

}  // namespace wezt::audio
