#include <algorithm>
#include <memory>

#include <gtest/gtest.h>

#include "clache/policy.h"

namespace clache {
namespace {

// Minimal policy that exercises the registration macro. Policies compiled into
// clache_core register the same way; the whole-archive link keeps them.
class NullPolicy final : public Policy {
 public:
  std::string_view name() const override { return "test_null"; }
  void on_hit(SlotId, TimePoint) override {}
  void plan_admission(SlotId, std::uint64_t, TimePoint, AdmissionPlan& plan) override {
    plan.admit = false;
    plan.decision = Decision::RejectPolicy;
  }
  void on_insert(SlotId, TimePoint) override {}
  void on_remove(SlotId, RemovalReason) override {}
  std::uint64_t metadata_bytes() const override { return 0; }
};

}  // namespace
}  // namespace clache

CLACHE_REGISTER_POLICY("test_null", [](const clache::PolicyConfig&, const clache::PolicyContext&) {
  return std::make_unique<clache::NullPolicy>();
})

namespace clache {
namespace {

TEST(PolicyRegistry, MacroRegistersAndFactoryBuilds) {
  EXPECT_TRUE(is_registered_policy("test_null"));
  const auto names = registered_policies();
  EXPECT_NE(std::find(names.begin(), names.end(), "test_null"), names.end());
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));

  const auto policy = make_policy("test_null", PolicyConfig{}, PolicyContext{});
  ASSERT_NE(policy, nullptr);
  EXPECT_EQ(policy->name(), "test_null");
  EXPECT_TRUE(std::isnan(policy->value(0, TimePoint{})));
}

TEST(PolicyRegistry, UnknownNameThrowsWithRegisteredList) {
  try {
    (void)make_policy("no_such_policy", PolicyConfig{}, PolicyContext{});
    FAIL() << "expected std::invalid_argument";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("test_null"), std::string::npos);
  }
}

TEST(PolicyRegistry, DuplicateAndEmptyRegistrationsThrow) {
  const PolicyFactory factory = [](const PolicyConfig&, const PolicyContext&) {
    return std::make_unique<NullPolicy>();
  };
  EXPECT_THROW(register_policy("test_null", factory), std::logic_error);
  EXPECT_THROW(register_policy("", factory), std::logic_error);
  EXPECT_THROW(register_policy("test_empty_factory", PolicyFactory{}), std::logic_error);
}

TEST(AdmissionPlan, ResetKeepsCapacity) {
  AdmissionPlan plan;
  plan.admit = true;
  plan.victims = {1, 2, 3};
  plan.victim_bytes = 30;
  const auto capacity = plan.victims.capacity();
  plan.reset();
  EXPECT_FALSE(plan.admit);
  EXPECT_TRUE(plan.victims.empty());
  EXPECT_EQ(plan.victims.capacity(), capacity);
  EXPECT_EQ(plan.victim_bytes, 0U);
  EXPECT_TRUE(std::isnan(plan.candidate_value));
}

}  // namespace
}  // namespace clache
