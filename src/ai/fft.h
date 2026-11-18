#pragma once
// Small in-place radix-2 FFT for analysis (not real-time). Size must be a power of two.
#include <cmath>
#include <complex>
#include <vector>

namespace wezt::ai {

class FFT {
 public:
  explicit FFT(std::size_t n) : n_(n), tw_(n / 2), rev_(n) {
    constexpr double kPi = 3.14159265358979323846;
    for (std::size_t i = 0; i < n / 2; ++i) tw_[i] = std::polar(1.0, -2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < n) ++bits;
    for (std::size_t i = 0; i < n; ++i) {
      std::size_t r = 0;
      for (std::size_t b = 0; b < bits; ++b) if (i & (std::size_t{1} << b)) r |= std::size_t{1} << (bits - 1 - b);
      rev_[i] = r;
    }
  }
  std::size_t size() const { return n_; }
  void forward(std::vector<std::complex<double>>& x) const {
    for (std::size_t i = 0; i < n_; ++i) if (i < rev_[i]) std::swap(x[i], x[rev_[i]]);
    for (std::size_t len = 2; len <= n_; len <<= 1) {
      const std::size_t step = n_ / len;
      for (std::size_t i = 0; i < n_; i += len)
        for (std::size_t k = 0; k < len / 2; ++k) {
          const auto u = x[i + k], v = x[i + k + len / 2] * tw_[k * step];
          x[i + k] = u + v;
          x[i + k + len / 2] = u - v;
        }
    }
  }

 private:
  std::size_t n_;
  std::vector<std::complex<double>> tw_;
  std::vector<std::size_t> rev_;
};

}  // namespace wezt::ai
