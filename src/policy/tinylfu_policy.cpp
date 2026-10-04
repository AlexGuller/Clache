// Byte-capacity Window TinyLFU, following Caffeine and Einziger et al.,
// "TinyLFU: A Highly Efficient Cache Admission Policy" (Spec §11.5).
// The doorkeeper and randomized warm-candidate admission are omitted.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "clache/hash.h"
#include "clache/policy.h"

#include "modern_queues.h"
namespace clache {
namespace {
class TinyLfuPolicy final : public Policy {
 public:
  TinyLfuPolicy(const PolicyConfig& c, const PolicyContext& ctx) : ctx_(ctx), q_(ctx.meta) {
    const auto hint = std::max<std::uint64_t>(
        16, ctx.expected_entries ? ctx.expected_entries : ctx.capacity_bytes / 4096);
    if (hint > (std::uint64_t{1} << 60)) throw std::invalid_argument("TinyLFU sketch too large");
    width_ = std::bit_ceil(hint);
    counters_.resize(static_cast<std::size_t>(2 * width_));
    window_ = static_cast<std::uint64_t>(static_cast<long double>(ctx.capacity_bytes) *
                                         c.tinylfu.window_fraction);
    protected_ = static_cast<std::uint64_t>(static_cast<long double>(ctx.capacity_bytes - window_) *
                                            c.tinylfu.protected_fraction);
  }
  std::string_view name() const override { return "wtinylfu"; }
  void on_access(SlotId s, TimePoint) override {
    auto indices = positions((*ctx_.meta)[s].key);
    unsigned minimum = 15;
    for (auto i : indices) minimum = std::min(minimum, get(i));
    if (minimum < 15)
      for (auto i : indices)
        if (get(i) == minimum) set(i, minimum + 1);
    if (++accesses_ == 10 * width_) {
      // Shift both packed nibbles without leaking the upper low bit.
      for (auto& b : counters_) b = static_cast<std::uint8_t>((b >> 1) & 0x77);
      accesses_ = 0;
    }
  }
  void on_hit(SlotId s, TimePoint) override {
    auto r = q_.nodes[s].region;
    q_.push(s, r == 2 ? 3 : r);
    while (q_.queues[3].bytes > protected_) q_.push(q_.queues[3].tail, 2);
  }
  void on_insert(SlotId s, TimePoint) override { q_.push(s, 1); }
  void on_remove(SlotId s, RemovalReason) override {
    q_.unlink(s);
    if (s < q_.nodes.size()) q_.nodes[s] = {};
  }
  void plan_admission(SlotId candidate, std::uint64_t needed, TimePoint,
                      AdmissionPlan& p) override {
    p.reset();
    p.admit = true;
    p.decision = Decision::AdmitWithEviction;
    while (p.victim_bytes < needed) {
      SlotId v = main_tail();
      if (q_.queues[1].head != kNoSlot &&
          (q_.queues[1].bytes > window_ ||
           (*ctx_.meta)[candidate].charged_bytes > window_ - q_.queues[1].bytes)) {
        SlotId c = q_.queues[1].tail;
        q_.unlink(c);
        if (v == kNoSlot) {
          q_.push(c, 2);
          continue;
        }
        if (estimate(c) > estimate(v)) {
          q_.unlink(v);
          q_.push(c, 2);
        } else
          v = c;
      } else {
        if (v == kNoSlot) v = q_.queues[1].tail;
        q_.unlink(v);
      }
      p.victims.push_back(v);
      p.victim_bytes += (*ctx_.meta)[v].charged_bytes;
    }
  }
  double value(SlotId s, TimePoint) const override { return estimate(s); }
  std::uint64_t metadata_bytes() const override { return q_.memory() + counters_.capacity(); }
  bool check_invariants() const override { return q_.check(); }

 private:
  SlotId main_tail() const {
    return q_.queues[2].tail != kNoSlot ? q_.queues[2].tail : q_.queues[3].tail;
  }
  std::array<std::uint64_t, 4> positions(KeyId key) const {
    std::array<std::uint64_t, 4> a{};
    for (std::uint64_t r = 0; r < 4; ++r) a[r] = r * width_ + (hash64_u64(key, r) & (width_ - 1));
    return a;
  }
  unsigned get(std::uint64_t i) const { return (counters_[i / 2] >> ((i % 2) * 4)) & 15U; }
  void set(std::uint64_t i, unsigned n) {
    const auto shift = static_cast<unsigned>((i % 2) * 4);
    auto& b = counters_[i / 2];
    b = static_cast<std::uint8_t>((b & ~(15U << shift)) | (n << shift));
  }
  unsigned estimate(SlotId s) const {
    unsigned n = 15;
    for (auto i : positions((*ctx_.meta)[s].key)) n = std::min(n, get(i));
    return n;
  }
  PolicyContext ctx_;
  modern::Queues q_;
  std::vector<std::uint8_t> counters_;
  std::uint64_t width_ = 0, accesses_ = 0, window_ = 0, protected_ = 0;
};
}  // namespace
CLACHE_REGISTER_POLICY("wtinylfu", [](const PolicyConfig& c, const PolicyContext& x) {
  return std::make_unique<TinyLfuPolicy>(c, x);
});
}  // namespace clache
