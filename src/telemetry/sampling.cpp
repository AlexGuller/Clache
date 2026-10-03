#include <cmath>

#include "clache/hash.h"
#include "clache/telemetry.h"

namespace clache {

bool should_sample(std::uint64_t seq, std::uint64_t seed, double rate) noexcept {
  if (!(rate > 0.0)) return false;
  if (rate >= 1.0) return true;
  // 2^64 * rate, computed in double; exact enough for sampling rates.
  const double threshold = std::ldexp(rate, 64);
  return static_cast<double>(hash64_u64(seq, seed)) < threshold;
}

}  // namespace clache
