// Deterministic random numbers (Spec §15.1).
//
// std::*_distribution output is implementation-defined and differs between
// libstdc++ and libc++, so Clache never uses it. Everything here is specified
// bit-for-bit (given the same libm for log/exp/cos/sin).
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace clache {

// SplitMix64 finalizer. A bijection on 64-bit integers.
constexpr std::uint64_t splitmix64_mix(std::uint64_t z) noexcept {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

class SplitMix64 {
 public:
  explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}
  constexpr std::uint64_t next() noexcept {
    state_ += 0x9e3779b97f4a7c15ULL;
    return splitmix64_mix(state_);
  }

 private:
  std::uint64_t state_;
};

// xoshiro256** 1.0 (Blackman & Vigna), seeded with four SplitMix64 outputs.
class Xoshiro256ss {
 public:
  explicit Xoshiro256ss(std::uint64_t seed) noexcept {
    SplitMix64 sm(seed);
    for (auto& word : s_) word = sm.next();
  }

  // Independent sub-stream per purpose, so adding a new stream never shifts
  // existing ones: Xoshiro256ss(splitmix64_mix(seed ^ stream_id)).
  static Xoshiro256ss for_stream(std::uint64_t seed, std::uint64_t stream_id) noexcept {
    return Xoshiro256ss(splitmix64_mix(seed ^ stream_id));
  }

  std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Uniform in [0, 1) with 53 bits of precision.
  double uniform01() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

  // Uniform integer in [0, bound), unbiased (Lemire's method). bound > 0.
  std::uint64_t uniform_below(std::uint64_t bound) noexcept {
    __extension__ using U128 = unsigned __int128;
    U128 m = static_cast<U128>(next()) * bound;
    auto low = static_cast<std::uint64_t>(m);
    if (low < bound) {
      const std::uint64_t threshold = (0 - bound) % bound;
      while (low < threshold) {
        m = static_cast<U128>(next()) * bound;
        low = static_cast<std::uint64_t>(m);
      }
    }
    return static_cast<std::uint64_t>(m >> 64);
  }

  bool bernoulli(double p) noexcept { return uniform01() < p; }

  // Standard normal via Box-Muller; the second value of each pair is cached.
  double normal() noexcept {
    if (has_spare_) {
      has_spare_ = false;
      return spare_;
    }
    const double u1 = 1.0 - uniform01();  // (0, 1], avoids log(0)
    const double u2 = uniform01();
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double theta = 2.0 * 3.14159265358979323846 * u2;
    spare_ = r * std::sin(theta);
    has_spare_ = true;
    return r * std::cos(theta);
  }

  // median * exp(sigma * N(0,1)).
  double lognormal(double median, double sigma) noexcept {
    return median * std::exp(sigma * normal());
  }

  // Exponential with the given rate (mean 1/rate).
  double exponential(double rate) noexcept { return -std::log1p(-uniform01()) / rate; }

  // Fisher-Yates shuffle (std::shuffle is implementation-defined).
  template <class RandomIt>
  void shuffle(RandomIt first, RandomIt last) noexcept {
    const auto n = static_cast<std::uint64_t>(std::distance(first, last));
    for (std::uint64_t i = n; i > 1; --i) {
      const std::uint64_t j = uniform_below(i);
      using std::swap;
      swap(first[static_cast<std::ptrdiff_t>(i - 1)], first[static_cast<std::ptrdiff_t>(j)]);
    }
  }

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
  }

  std::array<std::uint64_t, 4> s_{};
  double spare_ = 0.0;
  bool has_spare_ = false;
};

// Zipf(n, s) over ranks [0, n), rank 0 most popular: P(rank k) ∝ 1/(k+1)^s.
// Precomputed CDF + binary search; intended for n <= 1e7.
class ZipfTable {
 public:
  ZipfTable(std::uint64_t n, double s);
  std::uint64_t sample(Xoshiro256ss& rng) const noexcept;
  double probability(std::uint64_t rank) const noexcept;
  std::uint64_t size() const noexcept { return cdf_.size(); }

 private:
  std::vector<double> cdf_;
};

}  // namespace clache
