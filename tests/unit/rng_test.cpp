#include "clache/rng.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

namespace clache {
namespace {

// Golden values computed with an independent Python implementation of the
// reference algorithms. If these change, every generated trace changes.

TEST(SplitMix64, MatchesReferenceSequenceForSeedZero) {
  SplitMix64 sm(0);
  EXPECT_EQ(sm.next(), 0xe220a8397b1dcdafULL);
  EXPECT_EQ(sm.next(), 0x6e789e6aa1b965f4ULL);
  EXPECT_EQ(sm.next(), 0x06c45d188009454fULL);
}

TEST(SplitMix64, MixIsTheFinalizer) {
  EXPECT_EQ(splitmix64_mix(1), 0x5692161d100b05e5ULL);
  EXPECT_EQ(splitmix64_mix(0), 0ULL);
}

TEST(Xoshiro256ss, MatchesGoldenSequenceForSeedOne) {
  Xoshiro256ss rng(1);
  const std::array<std::uint64_t, 5> expected = {0xb3f2af6d0fc710c5ULL, 0x853b559647364ceaULL,
                                                 0x92f89756082a4514ULL, 0x642e1c7bc266a3a7ULL,
                                                 0xb27a48e29a233673ULL};
  for (const auto value : expected) EXPECT_EQ(rng.next(), value);
}

TEST(Xoshiro256ss, Uniform01MatchesGolden) {
  Xoshiro256ss rng(42);
  EXPECT_DOUBLE_EQ(rng.uniform01(), 0.08386297105988216);
  EXPECT_DOUBLE_EQ(rng.uniform01(), 0.3789802506626686);
  EXPECT_DOUBLE_EQ(rng.uniform01(), 0.6800434110281394);
}

TEST(Xoshiro256ss, UniformBelowMatchesGolden) {
  Xoshiro256ss rng(7);
  const std::array<std::uint64_t, 10> expected = {7, 2, 8, 9, 9, 8, 0, 1, 4, 1};
  for (const auto value : expected) EXPECT_EQ(rng.uniform_below(10), value);
}

TEST(Xoshiro256ss, UniformBelowStaysInRange) {
  Xoshiro256ss rng(3);
  for (int i = 0; i < 10000; ++i) {
    EXPECT_LT(rng.uniform_below(3), 3U);
    EXPECT_EQ(rng.uniform_below(1), 0U);
  }
}

TEST(Xoshiro256ss, StreamsAreDeterministicAndIndependent) {
  auto a1 = Xoshiro256ss::for_stream(99, 1);
  auto a2 = Xoshiro256ss::for_stream(99, 1);
  auto b = Xoshiro256ss::for_stream(99, 2);
  const auto first = a1.next();
  EXPECT_EQ(first, a2.next());
  EXPECT_NE(first, b.next());
}

TEST(Xoshiro256ss, NormalHasRoughlyUnitMoments) {
  Xoshiro256ss rng(5);
  constexpr int kN = 200000;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (int i = 0; i < kN; ++i) {
    const double x = rng.normal();
    sum += x;
    sum_sq += x * x;
  }
  const double mean = sum / kN;
  EXPECT_NEAR(mean, 0.0, 0.01);
  EXPECT_NEAR(sum_sq / kN - mean * mean, 1.0, 0.02);
}

TEST(Xoshiro256ss, ExponentialHasExpectedMean) {
  Xoshiro256ss rng(6);
  constexpr int kN = 200000;
  double sum = 0.0;
  for (int i = 0; i < kN; ++i) sum += rng.exponential(4.0);
  EXPECT_NEAR(sum / kN, 0.25, 0.005);
}

TEST(Xoshiro256ss, LognormalMedian) {
  Xoshiro256ss rng(8);
  std::vector<double> xs(100001);
  for (auto& x : xs) x = rng.lognormal(2000.0, 1.0);
  std::nth_element(xs.begin(), xs.begin() + 50000, xs.end());
  EXPECT_NEAR(xs[50000], 2000.0, 60.0);
}

TEST(Xoshiro256ss, ShuffleIsDeterministicPermutation) {
  std::vector<int> a(100);
  std::iota(a.begin(), a.end(), 0);
  auto b = a;
  Xoshiro256ss r1(11);
  Xoshiro256ss r2(11);
  r1.shuffle(a.begin(), a.end());
  r2.shuffle(b.begin(), b.end());
  EXPECT_EQ(a, b);
  auto sorted = a;
  std::sort(sorted.begin(), sorted.end());
  for (int i = 0; i < 100; ++i) EXPECT_EQ(sorted[static_cast<std::size_t>(i)], i);
}

TEST(ZipfTable, ProbabilitiesFollowPowerLaw) {
  const ZipfTable zipf(1000, 1.0);
  EXPECT_EQ(zipf.size(), 1000U);
  EXPECT_NEAR(zipf.probability(0) / zipf.probability(1), 2.0, 1e-9);
  EXPECT_NEAR(zipf.probability(0) / zipf.probability(9), 10.0, 1e-9);
  double total = 0.0;
  for (std::uint64_t k = 0; k < zipf.size(); ++k) total += zipf.probability(k);
  EXPECT_NEAR(total, 1.0, 1e-12);
  EXPECT_EQ(zipf.probability(1000), 0.0);
}

TEST(ZipfTable, SamplingMatchesProbabilities) {
  const ZipfTable zipf(100, 0.9);
  Xoshiro256ss rng(12);
  std::vector<int> counts(100, 0);
  constexpr int kN = 500000;
  for (int i = 0; i < kN; ++i) ++counts[zipf.sample(rng)];
  EXPECT_NEAR(static_cast<double>(counts[0]) / kN, zipf.probability(0), 0.005);
  EXPECT_NEAR(static_cast<double>(counts[10]) / kN, zipf.probability(10), 0.003);
}

TEST(ZipfTable, RejectsInvalidParameters) {
  EXPECT_THROW(ZipfTable(0, 1.0), std::logic_error);
  EXPECT_THROW(ZipfTable(10, -1.0), std::logic_error);
}

}  // namespace
}  // namespace clache
