#include <limits>

#include "clache/check.h"
#include "clache/metadata.h"

namespace clache {

MetadataStore::MetadataStore(const ObservationConfig& config) : config_(config) {}

void MetadataStore::reserve(std::size_t slots) {
  CLACHE_REQUIRE(slots <= kNoSlot, "too many metadata slots");
  slots_.reserve(slots);
  index_.reserve(slots);
  free_list_.reserve(slots);
}

SlotId MetadataStore::find(KeyId key) const noexcept {
  const auto it = index_.find(key);
  return it == index_.end() ? kNoSlot : it->second;
}

bool MetadataStore::stale(SlotId slot, TimePoint now) const {
  // Compare using unsigned differences to avoid overflowing signed timestamps.
  const auto last = slots_[slot].last_access.ns();
  return now.ns() >= last &&
         static_cast<std::uint64_t>(now.ns()) - static_cast<std::uint64_t>(last) >=
             static_cast<std::uint64_t>(config_.ttl_seconds) * 1'000'000'000ULL;
}

SlotId MetadataStore::lookup_or_observe(KeyId key, TimePoint now) {
  SlotId slot = find(key);
  if (slot == kNoSlot) {
    if (free_list_.empty()) {
      CLACHE_REQUIRE(slots_.size() < kNoSlot, "metadata slot space exhausted");
      slot = static_cast<SlotId>(slots_.size());
      slots_.emplace_back();
    } else {
      slot = free_list_.back();
      free_list_.pop_back();
    }
    slots_[slot] = EntryMetadata{};
    slots_[slot].key = key;
    slots_[slot].in_use = true;
    slots_[slot].last_access = now;
    index_.emplace(key, slot);
    ++observed_count_;
    touch_observation(slot);
  } else if (!slots_[slot].resident) {
    if (stale(slot, now)) {
      slots_[slot].freq = FrequencyState{};
      slots_[slot].cost = CostState{};
      slots_[slot].access_count = 0;
    }
    touch_observation(slot);
  }
  // Never remove the returned slot, nor a pinned slot. Bound opportunistic work.
  for (unsigned dropped = 0; dropped < 2; ++dropped) {
    if (obs_tail_ == kNoSlot || obs_tail_ == slot || slots_[obs_tail_].pins != 0 ||
        !stale(obs_tail_, now)) {
      break;
    }
    drop_observation(obs_tail_);
  }
  return slot;
}

EntryMetadata& MetadataStore::operator[](SlotId slot) {
  CLACHE_REQUIRE(slot < slots_.size() && slots_[slot].in_use, "invalid metadata slot");
  return slots_[slot];
}
const EntryMetadata& MetadataStore::operator[](SlotId slot) const {
  CLACHE_REQUIRE(slot < slots_.size() && slots_[slot].in_use, "invalid metadata slot");
  return slots_[slot];
}

void MetadataStore::mark_resident(SlotId slot) {
  auto& entry = (*this)[slot];
  if (entry.resident) return;
  unlink_observation(slot);
  entry.resident = true;
  --observed_count_;
  ++resident_count_;
}

void MetadataStore::mark_nonresident(SlotId slot, TimePoint now) {
  auto& entry = (*this)[slot];
  if (entry.resident) {
    entry.resident = false;
    --resident_count_;
    ++observed_count_;
  }
  // Becoming a ghost touches the LRU list, without changing access telemetry.
  (void)now;
  touch_observation(slot);
}

void MetadataStore::pin(SlotId slot) {
  auto& entry = (*this)[slot];
  CLACHE_REQUIRE(entry.pins < std::numeric_limits<std::uint16_t>::max(), "pin count overflow");
  ++entry.pins;
}
void MetadataStore::unpin(SlotId slot) {
  auto& entry = (*this)[slot];
  CLACHE_REQUIRE(entry.pins != 0, "unpin without pin");
  --entry.pins;
}

std::size_t MetadataStore::resident_count() const noexcept { return resident_count_; }
std::size_t MetadataStore::observed_count() const noexcept { return observed_count_; }
std::size_t MetadataStore::tracked_count() const noexcept { return index_.size(); }
std::uint64_t MetadataStore::metadata_bytes() const noexcept {
  return slots_.capacity() * sizeof(EntryMetadata) + index_.bucket_count() * sizeof(void*) +
         index_.size() * (sizeof(std::pair<const KeyId, SlotId>) + 2 * sizeof(void*));
}

bool MetadataStore::check_invariants() const {
  std::vector<bool> seen(slots_.size(), false);
  std::size_t observations = 0;
  SlotId prev = kNoSlot;
  for (SlotId slot = obs_head_; slot != kNoSlot; slot = slots_[slot].obs_next) {
    if (slot >= slots_.size() || seen[slot]) return false;
    const auto& entry = slots_[slot];
    if (!entry.in_use || entry.resident || entry.obs_prev != prev) return false;
    seen[slot] = true;
    prev = slot;
    ++observations;
  }
  if (prev != obs_tail_ || observations != observed_count_) return false;
  std::size_t residents = 0;
  for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
    const auto& entry = slots_[slot];
    if (!entry.in_use) continue;
    if (find(entry.key) != slot) return false;
    if (entry.resident) {
      if (seen[slot] || entry.obs_prev != kNoSlot || entry.obs_next != kNoSlot) return false;
      seen[slot] = true;
      ++residents;
    } else if (!seen[slot]) {
      return false;
    }
  }
  if (residents != resident_count_ || residents + observations != index_.size()) return false;
  for (SlotId slot : free_list_) {
    if (slot >= slots_.size() || seen[slot] || slots_[slot].in_use) return false;
    seen[slot] = true;
  }
  for (bool present : seen) {
    if (!present) return false;
  }
  for (const auto& [key, slot] : index_) {
    if (slot >= slots_.size() || !slots_[slot].in_use || slots_[slot].key != key) return false;
  }
  return true;
}

}  // namespace clache
