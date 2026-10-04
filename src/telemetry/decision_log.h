#pragma once

#include <span>

#include "clache/config.h"
#include "clache/estimators.h"
#include "clache/metadata.h"
#include "clache/policy.h"
#include "clache/telemetry.h"

namespace clache::detail {
void emit_decision(DecisionSink* sink, const EngineConfig& config, const EntryMetadata& entry,
                   const FrequencyEstimator& frequency, const MissCostEstimator& cost,
                   const Policy& policy, SlotId slot, const AdmissionPlan& plan, Decision decision,
                   TimePoint now, std::uint64_t bytes_needed, std::span<const KeyId> victims);
}  // namespace clache::detail
