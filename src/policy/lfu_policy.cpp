#include <memory>

#include "classic_heap_policy.h"

namespace clache {
CLACHE_REGISTER_POLICY("lfu", [](const PolicyConfig& c, const PolicyContext& ctx) {
  return std::make_unique<detail::ClassicHeapPolicy>(detail::ClassicHeapPolicy::Kind::Lfu, c, ctx);
});
CLACHE_REGISTER_POLICY("lfu_da", [](const PolicyConfig& c, const PolicyContext& ctx) {
  return std::make_unique<detail::ClassicHeapPolicy>(detail::ClassicHeapPolicy::Kind::LfuDa, c,
                                                     ctx);
});
}  // namespace clache
