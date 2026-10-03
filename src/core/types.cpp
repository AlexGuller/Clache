#include "clache/types.h"

namespace clache {

std::string_view to_string(Decision d) noexcept {
  switch (d) {
    case Decision::Hit:
      return "HIT";
    case Decision::AdmitFreeSpace:
      return "ADMIT_FREE_SPACE";
    case Decision::AdmitWithEviction:
      return "ADMIT_WITH_EVICTION";
    case Decision::RejectOversized:
      return "OVERSIZED";
    case Decision::RejectInsufficientHistory:
      return "INSUFFICIENT_HISTORY";
    case Decision::RejectCandidateNotHigher:
      return "CANDIDATE_VALUE_NOT_HIGHER_THAN_REQUIRED_VICTIMS";
    case Decision::RejectTooManyVictims:
      return "TOO_MANY_VICTIMS";
    case Decision::RejectInvalidatedDuringLoad:
      return "INVALIDATED_DURING_LOAD";
    case Decision::RejectHashCollision:
      return "HASH_COLLISION";
    case Decision::RejectPolicy:
      return "POLICY_REJECT";
  }
  return "UNKNOWN";
}

std::string_view decision_class(Decision d) noexcept {
  if (d == Decision::Hit) return "HIT";
  return is_admit(d) ? "ADMIT" : "REJECT";
}

std::string_view to_string(RemovalReason r) noexcept {
  switch (r) {
    case RemovalReason::Evicted:
      return "EVICTED";
    case RemovalReason::Expired:
      return "EXPIRED";
    case RemovalReason::Invalidated:
      return "INVALIDATED";
    case RemovalReason::Replaced:
      return "REPLACED";
  }
  return "UNKNOWN";
}

}  // namespace clache
