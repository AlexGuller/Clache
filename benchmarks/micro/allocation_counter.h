#pragma once

#include <cstdint>

namespace clache::bench {
// Counts allocations on this benchmark thread only; setup is explicitly excluded.
void begin_allocations() noexcept;
std::uint64_t end_allocations() noexcept;
}  // namespace clache::bench
