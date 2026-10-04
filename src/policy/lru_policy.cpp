#include <list>
#include <optional>
#include <vector>

#include "clache/policy.h"

namespace clache {
namespace {
class LruPolicy final : public Policy {
 public:
  explicit LruPolicy(const PolicyContext& context) : meta_(*context.meta) {}
  std::string_view name() const override { return "lru"; }
  void on_hit(SlotId slot, TimePoint) override {
    order_.splice(order_.end(), order_, *positions_[slot]);
  }
  void plan_admission(SlotId, std::uint64_t needed, TimePoint, AdmissionPlan& plan) override {
    plan.reset();
    for (SlotId slot : order_) {
      plan.victims.push_back(slot);
      plan.victim_bytes += meta_[slot].charged_bytes;
      if (plan.victim_bytes >= needed) break;
    }
    plan.admit = plan.victim_bytes >= needed;
    plan.decision = Decision::AdmitWithEviction;
  }
  void on_insert(SlotId slot, TimePoint) override {
    order_.push_back(slot);
    if (positions_.size() <= slot) positions_.resize(static_cast<std::size_t>(slot) + 1);
    positions_[slot] = std::prev(order_.end());
  }
  void on_remove(SlotId slot, RemovalReason) override {
    order_.erase(*positions_[slot]);
    positions_[slot].reset();
  }
  std::uint64_t metadata_bytes() const override {
    return positions_.capacity() * sizeof(positions_[0]) +
           order_.size() * (sizeof(SlotId) + 2 * sizeof(void*));
  }
  bool check_invariants() const override {
    if (order_.size() != meta_.resident_count()) return false;
    std::size_t tracked = 0;
    for (const auto& position : positions_)
      if (position) ++tracked;
    if (tracked != order_.size()) return false;
    for (auto it = order_.begin(); it != order_.end(); ++it) {
      if (!meta_[*it].resident || *it >= positions_.size() || !positions_[*it] ||
          *positions_[*it] != it)
        return false;
    }
    return true;
  }

 private:
  const MetadataStore& meta_;
  std::list<SlotId> order_;
  std::vector<std::optional<std::list<SlotId>::iterator>> positions_;
};
}  // namespace
CLACHE_REGISTER_POLICY("lru", [](const PolicyConfig&, const PolicyContext& context) {
  return std::make_unique<LruPolicy>(context);
});
}  // namespace clache
