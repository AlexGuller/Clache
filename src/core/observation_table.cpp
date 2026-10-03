#include "clache/metadata.h"

namespace clache {

void MetadataStore::unlink_observation(SlotId slot) {
  auto& entry = slots_[slot];
  if (entry.obs_prev != kNoSlot)
    slots_[entry.obs_prev].obs_next = entry.obs_next;
  else if (obs_head_ == slot)
    obs_head_ = entry.obs_next;
  if (entry.obs_next != kNoSlot)
    slots_[entry.obs_next].obs_prev = entry.obs_prev;
  else if (obs_tail_ == slot)
    obs_tail_ = entry.obs_prev;
  entry.obs_prev = entry.obs_next = kNoSlot;
}

void MetadataStore::touch_observation(SlotId slot) {
  unlink_observation(slot);
  slots_[slot].obs_next = obs_head_;
  if (obs_head_ != kNoSlot)
    slots_[obs_head_].obs_prev = slot;
  else
    obs_tail_ = slot;
  obs_head_ = slot;
}

void MetadataStore::drop_observation(SlotId slot) {
  unlink_observation(slot);
  index_.erase(slots_[slot].key);
  slots_[slot] = EntryMetadata{};
  free_list_.push_back(slot);
  --observed_count_;
}

void MetadataStore::trim() {
  SlotId slot = obs_tail_;
  while (observed_count_ > config_.max_entries && slot != kNoSlot) {
    const SlotId previous = slots_[slot].obs_prev;
    if (slots_[slot].pins == 0) drop_observation(slot);
    slot = previous;
  }
}

}  // namespace clache
