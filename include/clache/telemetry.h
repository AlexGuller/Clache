// Sampled decision records for explainability (Spec §17.2).
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "clache/types.h"

namespace clache {

struct DecisionRecord {
  std::uint64_t seq = 0;
  TimePoint now{};
  KeyId key = 0;
  Decision decision = Decision::Hit;
  std::string_view policy;  // points at Policy::name(); valid for the policy's lifetime
  double candidate_value = 0.0;
  double threshold_value = 0.0;
  double frequency_rps = 0.0;
  double miss_cost_ms = 0.0;
  std::uint64_t size_bytes = 0;
  std::uint64_t bytes_needed = 0;
  std::uint32_t victim_count = 0;
  std::array<KeyId, 8> victims_head{};  // first min(victim_count, 8) victims
};

class DecisionSink {
 public:
  virtual ~DecisionSink() = default;
  virtual void record(const DecisionRecord& record) = 0;
};

// Deterministic sampling: true iff hash64_u64(seq, seed) < rate * 2^64.
bool should_sample(std::uint64_t seq, std::uint64_t seed, double rate) noexcept;

}  // namespace clache
