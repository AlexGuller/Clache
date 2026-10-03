#include "clache/rng.h"

#include <algorithm>

#include "clache/check.h"

namespace clache {

ZipfTable::ZipfTable(std::uint64_t n, double s) {
  CLACHE_REQUIRE(n > 0, "ZipfTable needs n > 0");
  CLACHE_REQUIRE(s >= 0.0, "ZipfTable needs s >= 0");
  cdf_.resize(n);
  // Fixed summation order keeps the table bit-identical across runs.
  double sum = 0.0;
  for (std::uint64_t k = 0; k < n; ++k) {
    sum += 1.0 / std::pow(static_cast<double>(k + 1), s);
    cdf_[k] = sum;
  }
  for (auto& c : cdf_) c /= sum;
  cdf_.back() = 1.0;
}

std::uint64_t ZipfTable::sample(Xoshiro256ss& rng) const noexcept {
  const double u = rng.uniform01();
  const auto it = std::upper_bound(cdf_.begin(), cdf_.end(), u);
  const auto rank = static_cast<std::uint64_t>(it - cdf_.begin());
  return std::min<std::uint64_t>(rank, cdf_.size() - 1);
}

double ZipfTable::probability(std::uint64_t rank) const noexcept {
  if (rank >= cdf_.size()) return 0.0;
  return rank == 0 ? cdf_[0] : cdf_[rank] - cdf_[rank - 1];
}

}  // namespace clache
