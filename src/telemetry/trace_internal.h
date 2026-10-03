#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

#include "clache/trace.h"
namespace clache::trace_detail {
[[noreturn]] inline void fail(const std::filesystem::path& path, std::uint64_t line,
                              const std::string& message) {
  throw TraceError(path.string() + ":" + std::to_string(line) + ": " + message);
}
inline void validate(const TraceRecord& r, std::uint64_t previous,
                     const std::filesystem::path& path, std::uint64_t line) {
  if (r.timestamp_ns < previous) fail(path, line, "timestamp_ns decreases");
  if (r.op != TraceOp::Get && r.op != TraceOp::Del) fail(path, line, "invalid op");
  if (r.op == TraceOp::Get && r.size_bytes == 0)
    fail(path, line, "get size_bytes must be positive");
}
inline std::uint64_t decode(const unsigned char* p, unsigned n) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return v;
}
inline void encode(unsigned char* p, std::uint64_t v, unsigned n) {
  for (unsigned i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
}  // namespace clache::trace_detail
