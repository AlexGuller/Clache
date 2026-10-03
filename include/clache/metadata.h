// Per-key metadata and the bounded observation (ghost) table (Spec §7).
// Implemented on feat/metadata-store.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "clache/config.h"
#include "clache/estimators.h"
#include "clache/types.h"

namespace clache {

struct EntryMetadata {
  KeyId key = 0;
  std::uint64_t charged_bytes = 0;
  FrequencyState freq;
  CostState cost;
  TimePoint last_access{};
  std::uint64_t last_access_seq = 0;  // engine request sequence number; unique per access
  TimePoint expiry = kNever;
  std::uint32_t access_count = 0;  // accesses since slot creation / telemetry reset (saturating)
  std::uint32_t generation = 0;    // bumped by invalidate()
  SlotId obs_prev = kNoSlot;       // intrusive observation list (MRU = head)
  SlotId obs_next = kNoSlot;
  std::uint16_t pins = 0;  // >0: never dropped from the observation table
  bool resident = false;
  bool in_use = false;
};
static_assert(sizeof(EntryMetadata) <= 128, "keep per-key metadata within budget (Spec §7.1)");

// Owns every tracked key: residents plus non-resident observation entries.
//
// Observation-table rules (Spec §7.3):
//  * Non-resident in-use slots form an LRU list ordered by most recent touch
//    (creation, access, or becoming non-resident).
//  * Residents and pinned slots are never dropped.
//  * lookup_or_observe() never drops the slot it returns, so observed_count()
//    may exceed max_entries by the slots touched during the current engine
//    operation. trim() restores the strict bound; the engine calls it at the
//    end of every public operation.
//  * max_entries == 0 disables ghosts: trim() drops every unpinned
//    non-resident slot.
//  * Dropped slots go on a LIFO free list, so slot reuse is deterministic.
class MetadataStore {
 public:
  explicit MetadataStore(const ObservationConfig& config);

  void reserve(std::size_t slots);

  // kNoSlot if the key is not tracked.
  SlotId find(KeyId key) const noexcept;

  // Returns the key's slot, creating a non-resident entry if needed, and
  // touches it (non-resident: moves to observation MRU). If the slot is
  // non-resident and last_access + ttl <= now, its telemetry (freq, cost,
  // access_count) is reset first; generation is kept. Does NOT record the
  // access; the engine does that. May drop up to 2 stale tail entries.
  SlotId lookup_or_observe(KeyId key, TimePoint now);

  EntryMetadata& operator[](SlotId slot);
  const EntryMetadata& operator[](SlotId slot) const;

  // Non-resident -> resident: unlinks from the observation list.
  void mark_resident(SlotId slot);
  // Resident -> non-resident (evicted / expired / invalidated / replaced):
  // links at observation MRU. Telemetry is kept.
  void mark_nonresident(SlotId slot, TimePoint now);

  // Pinned slots survive trim(). Cache pins a key between lookup() and
  // complete_miss()/abort_miss() while its loader runs unlocked.
  void pin(SlotId slot);
  void unpin(SlotId slot);

  // Enforces observed_count() <= max_entries by dropping unpinned LRU tail
  // entries.
  void trim();

  std::size_t resident_count() const noexcept;
  std::size_t observed_count() const noexcept;
  std::size_t tracked_count() const noexcept;

  // Spec §7.4 accounting formula.
  std::uint64_t metadata_bytes() const noexcept;

  bool check_invariants() const;

 private:
  ObservationConfig config_;
  std::vector<EntryMetadata> slots_;
  std::unordered_map<KeyId, SlotId> index_;
  std::vector<SlotId> free_list_;
  SlotId obs_head_ = kNoSlot;
  SlotId obs_tail_ = kNoSlot;
  std::size_t resident_count_ = 0;
  std::size_t observed_count_ = 0;
};

}  // namespace clache
