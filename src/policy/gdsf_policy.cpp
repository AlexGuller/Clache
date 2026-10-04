#include <memory>

#include "classic_heap_policy.h"

namespace clache {
CLACHE_REGISTER_POLICY("gds", [](const PolicyConfig& c, const PolicyContext& ctx) {
  return std::make_unique<detail::ClassicHeapPolicy>(detail::ClassicHeapPolicy::Kind::Gds, c, ctx);
});
CLACHE_REGISTER_POLICY("gdsf", [](const PolicyConfig& c, const PolicyContext& ctx) {
  return std::make_unique<detail::ClassicHeapPolicy>(detail::ClassicHeapPolicy::Kind::Gdsf, c, ctx);
});
CLACHE_REGISTER_POLICY("gdsf_window", [](const PolicyConfig& c, const PolicyContext& ctx) {
  return std::make_unique<detail::ClassicHeapPolicy>(detail::ClassicHeapPolicy::Kind::GdsfWindow, c,
                                                     ctx);
});
}  // namespace clache
