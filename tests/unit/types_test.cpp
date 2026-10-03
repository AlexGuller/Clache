#include "clache/types.h"

#include <set>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "clache/check.h"
#include "clache/hash.h"
#include "clache/telemetry.h"

namespace clache {
namespace {

TEST(TimePoint, ArithmeticAndOrdering) {
  const auto a = TimePoint::from_ns(1'000);
  const auto b = a + Nanos{500};
  EXPECT_EQ(b.ns(), 1'500);
  EXPECT_EQ((b - a).count(), 500);
  EXPECT_LT(a, b);
  EXPECT_LT(b, kNever);
  EXPECT_EQ(a, TimePoint::from_ns(1'000));
  EXPECT_DOUBLE_EQ(to_seconds(Nanos{1'500'000'000}), 1.5);
  EXPECT_DOUBLE_EQ(us_to_ms(240'000), 240.0);
}

TEST(Decision, ClassificationAndUniqueNames) {
  std::set<std::string> names;
  for (std::size_t i = 0; i < kDecisionCount; ++i) {
    const auto d = static_cast<Decision>(i);
    const auto name = std::string(to_string(d));
    EXPECT_NE(name, "UNKNOWN");
    names.insert(name);
    EXPECT_NE(is_admit(d), is_reject(d) || d == Decision::Hit) << name;
  }
  EXPECT_EQ(names.size(), kDecisionCount);
  EXPECT_EQ(to_string(Decision::RejectCandidateNotHigher),
            "CANDIDATE_VALUE_NOT_HIGHER_THAN_REQUIRED_VICTIMS");
  EXPECT_EQ(decision_class(Decision::Hit), "HIT");
  EXPECT_EQ(decision_class(Decision::AdmitWithEviction), "ADMIT");
  EXPECT_EQ(decision_class(Decision::RejectOversized), "REJECT");
}

TEST(RemovalReason, Names) {
  for (std::size_t i = 0; i < kRemovalReasonCount; ++i) {
    EXPECT_NE(to_string(static_cast<RemovalReason>(i)), "UNKNOWN");
  }
}

TEST(Check, RequireThrowsLogicError) {
  EXPECT_THROW(CLACHE_REQUIRE(1 + 1 == 3, "math"), std::logic_error);
  EXPECT_NO_THROW(CLACHE_REQUIRE(true, "fine"));
}

TEST(Hash, Xxh3KnownValueAndDeterminism) {
  // XXH3_64bits("") with seed 0, from the xxHash reference.
  EXPECT_EQ(hash64(std::string_view{}), 0x2D06800538D394C2ULL);
  EXPECT_EQ(hash64("clache"), hash64("clache"));
  EXPECT_NE(hash64("clache"), hash64("clache", 1));
  EXPECT_NE(hash64_u64(1), hash64_u64(2));
}

TEST(Hash, StreamingMatchesOneShot) {
  Hasher64 h;
  h.update("cla");
  h.update("che");
  EXPECT_EQ(h.digest(), hash64("clache"));
}

TEST(Sampling, DeterministicAndRoughlyAtRate) {
  EXPECT_FALSE(should_sample(1, 0, 0.0));
  EXPECT_TRUE(should_sample(1, 0, 1.0));
  int hits = 0;
  for (std::uint64_t seq = 0; seq < 100000; ++seq) {
    const bool s = should_sample(seq, 42, 0.01);
    EXPECT_EQ(s, should_sample(seq, 42, 0.01));
    hits += s ? 1 : 0;
  }
  EXPECT_NEAR(hits, 1000, 150);
}

}  // namespace
}  // namespace clache
