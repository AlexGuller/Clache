// TEMPORARY TEST-ONLY STUB. DELETE THIS FILE when feat/metadata-store
// (Person A) is merged: the build will then fail with duplicate symbols for
// MetadataStore, which is the signal.
//
// Exists only so policy branches can unit-test before the real store lands.
// Simplified: no observation LRU list, no bound or TTL enforcement (trim() is
// a no-op). Tests may rely only on: lookup_or_observe, find, operator[],
// mark_resident, mark_nonresident, pin/unpin, and the counts.

#include <cstdint>

#include "clache/metadata.h"

namespace clache {

MetadataStore::MetadataStore(const ObservationConfig& config) : config_(config) {}

void MetadataStore::reserve(std::size_t slots) {
  slots_.reserve(slots);
  index_.reserve(slots);
}

SlotId MetadataStore::find(KeyId key) const noexcept {
  const auto it = index_.find(key);
  return it == index_.end() ? kNoSlot : it->second;
}

SlotId MetadataStore::lookup_or_observe(KeyId key, TimePoint now) {
  if (const SlotId existing = find(key); existing != kNoSlot) return existing;
  SlotId slot;
  if (!free_list_.empty()) {
    slot = free_list_.back();
    free_list_.pop_back();
  } else {
    slot = static_cast<SlotId>(slots_.size());
    slots_.emplace_back();
  }
  EntryMetadata& m = slots_[slot];
  m = EntryMetadata{};
  m.key = key;
  m.last_access = now;
  m.in_use = true;
  index_.emplace(key, slot);
  ++observed_count_;
  return slot;
}

EntryMetadata& MetadataStore::operator[](SlotId slot) { return slots_[slot]; }

const EntryMetadata& MetadataStore::operator[](SlotId slot) const { return slots_[slot]; }

void MetadataStore::mark_resident(SlotId slot) {
  EntryMetadata& m = slots_[slot];
  if (m.resident) return;
  m.resident = true;
  ++resident_count_;
  --observed_count_;
}

void MetadataStore::mark_nonresident(SlotId slot, TimePoint /*now*/) {
  EntryMetadata& m = slots_[slot];
  if (!m.resident) return;
  m.resident = false;
  --resident_count_;
  ++observed_count_;
}

void MetadataStore::pin(SlotId slot) { ++slots_[slot].pins; }

void MetadataStore::unpin(SlotId slot) { --slots_[slot].pins; }

void MetadataStore::trim() {}

std::size_t MetadataStore::resident_count() const noexcept { return resident_count_; }

std::size_t MetadataStore::observed_count() const noexcept { return observed_count_; }

std::size_t MetadataStore::tracked_count() const noexcept { return index_.size(); }

std::uint64_t MetadataStore::metadata_bytes() const noexcept {
  return slots_.capacity() * sizeof(EntryMetadata);
}

bool MetadataStore::check_invariants() const {
  // The stub keeps no observation list, so its head/tail stay unset.
  return resident_count_ + observed_count_ == index_.size() && obs_head_ == kNoSlot &&
         obs_tail_ == kNoSlot;
}

}  // namespace clache
