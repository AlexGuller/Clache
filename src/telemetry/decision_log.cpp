#include "telemetry/decision_log.h"

#include <algorithm>
#include <cmath>

namespace clache::detail {
void emit_decision(DecisionSink* sink, const EngineConfig& config, const EntryMetadata& entry,
                   const FrequencyEstimator& frequency, const MissCostEstimator& cost,
                   const Policy& policy, SlotId slot, const AdmissionPlan& plan, Decision decision,
                   TimePoint now, std::uint64_t bytes_needed, std::span<const KeyId> victims) {
  if (!sink || (decision == Decision::Hit && !config.telemetry.sample_hits) ||
      !should_sample(entry.last_access_seq, config.seed, config.telemetry.decision_sample_rate))
    return;
  DecisionRecord record;
  record.seq = entry.last_access_seq;
  record.now = now;
  record.key = entry.key;
  record.decision = decision;
  record.policy = policy.name();
  record.candidate_value =
      std::isnan(plan.candidate_value) ? policy.value(slot, now) : plan.candidate_value;
  record.threshold_value = plan.threshold_value;
  record.frequency_rps = frequency.rate(entry.freq, now);
  record.miss_cost_ms = cost.estimate_ms(entry.cost);
  record.size_bytes = entry.charged_bytes;
  record.bytes_needed = bytes_needed;
  record.victim_count = static_cast<std::uint32_t>(victims.size());
  std::copy_n(victims.begin(), std::min(victims.size(), record.victims_head.size()),
              record.victims_head.begin());
  sink->record(record);
}
}  // namespace clache::detail
