// Deterministic synthetic traces for the eight Spec §15 workloads.
#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "clache/trace.h"
namespace clache {
struct TracegenConfig {
  std::string workload = "zipf";
  std::map<std::string, std::string> params;
};
std::vector<std::string> tracegen_workloads();
std::map<std::string, std::string> tracegen_defaults(std::string_view workload);
// Validates every option before emitting records. The callback can throw.
TraceMeta generate_trace_records(const TracegenConfig& config,
                                 const std::function<void(const TraceRecord&)>& emit);
// Writes the trace and <path>.meta.json. Caller must provide an existing parent directory.
TraceMeta generate_trace(const TracegenConfig& config, const std::filesystem::path& path,
                         TraceFormat format = TraceFormat::Csv);
}  // namespace clache
